#include "compile.h"

#include "parser/parser.h"
#include "shady_scheduler_src.h"
#include "transform/internal_constants.h"
#include "ir_private.h"

#define KiB * 1024
#define MiB * 1024 KiB

CompilerConfig default_compiler_config() {
    return (CompilerConfig) {
        .allow_frontend_syntax = false,
        .dynamic_scheduling = true,
        .per_thread_stack_size = 1 KiB,
        .per_subgroup_stack_size = 1 KiB,

        .subgroup_size = 32,

        .target_spirv_version = {
            .major = 1,
            .minor = 4
        },
    };
}

ArenaConfig default_arena_config() {
    return (ArenaConfig) {
        .is_simt = true,
    };
}

#define mod (*pmod)

CompilationResult run_compiler_passes(CompilerConfig* config, Module** pmod) {
    ArenaConfig aconfig = get_module_arena(mod)->config;
    Module* old_mod;

    IrArena* old_arena = NULL;
    IrArena* tmp_arena = NULL;

    generate_dummy_constants(config, mod);

    aconfig.name_bound = true;
    RUN_PASS(bind_program)
    RUN_PASS(normalize)

    // TODO: do this late
    patch_constants(config, mod);

    aconfig.check_types = true;
    RUN_PASS(infer_program)

    aconfig.allow_fold = true;

    RUN_PASS(setup_stack_frames)
    RUN_PASS(mark_leaf_functions)

    RUN_PASS(lower_cf_instrs)
    RUN_PASS(opt_restructurize)

    RUN_PASS(lower_callf)
    RUN_PASS(opt_simplify_cf)

    RUN_PASS(lower_continuations)

    RUN_PASS(opt_simplify_cf)
    RUN_PASS(opt_stack)

    RUN_PASS(lower_tailcalls)

    RUN_PASS(eliminate_constants)

    aconfig.subgroup_mask_representation = SubgroupMaskInt64;
    RUN_PASS(lower_mask)

    RUN_PASS(lower_subgroup_ops)
    RUN_PASS(lower_stack)
    RUN_PASS(lower_physical_ptrs)
    RUN_PASS(lower_subgroup_vars)

    RUN_PASS(lower_int)

    if (config->lower.simt_to_explicit_simd) {
        aconfig.is_simt = false;
        RUN_PASS(simt2d)
    }

    return CompilationNoError;
}

#undef mod

CompilationResult parse_files(CompilerConfig* config, size_t num_files, const char** files_contents, Module* mod) {
        ParserConfig pconfig = {
            .front_end = config->allow_frontend_syntax
        };
    for (size_t i = 0; i < num_files; i++) {
        const char* input_file_contents = files_contents[i];

        debugv_print("Parsing: \n%s\n", input_file_contents);
        parse(pconfig, input_file_contents, mod);
    }

    if (config->dynamic_scheduling) {
        debugv_print("Parsing builtin scheduler code");
        parse(pconfig, shady_scheduler_src, mod);
    }

    return CompilationNoError;
}
