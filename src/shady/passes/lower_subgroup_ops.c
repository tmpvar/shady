#include "passes.h"

#include "portability.h"
#include "log.h"

#include "../rewrite.h"
#include "../type.h"
#include "../transform/ir_gen_helpers.h"
#include "../transform/memory_layout.h"

typedef struct {
    Rewriter rewriter;
    const CompilerConfig* config;
} Context;

static bool is_extended_type(SHADY_UNUSED IrArena* a, const Type* t, bool allow_vectors) {
    switch (t->tag) {
        case Int_TAG: return true;
        // TODO allow 16-bit floats specifically !
        case Float_TAG: return true;
        case PackType_TAG:
            if (allow_vectors)
                return is_extended_type(a, t->payload.pack_type.element_type, false);
            return false;
        default: return false;
    }
}

static const Node* process_let(Context* ctx, const Node* old) {
    assert(old->tag == Let_TAG);
    IrArena* a = ctx->rewriter.dst_arena;
    const Node* tail = rewrite_node(&ctx->rewriter, old->payload.let.tail);
    const Node* old_instruction = old->payload.let.instruction;

    if (old_instruction->tag == PrimOp_TAG) {
        PrimOp payload = old_instruction->payload.prim_op;
        switch (payload.op) {
            case subgroup_broadcast_first_op: {
                BodyBuilder* builder = begin_body(a);
                const Node* varying_value = rewrite_node(&ctx->rewriter, payload.operands.nodes[0]);
                const Type* element_type = get_unqualified_type(varying_value->type);

                if (element_type->tag == Int_TAG && element_type->payload.int_type.width == IntTy32) {
                    cancel_body(builder);
                    break;
                } else if (is_extended_type(a, element_type, true) && !ctx->config->lower.emulate_subgroup_ops_extended_types) {
                    cancel_body(builder);
                    break;
                }

                TypeMemLayout layout = get_mem_layout(a, element_type);

                const Type* local_arr_ty = arr_type(a, (ArrType) { .element_type = int32_type(a), .size = NULL });

                const Node* varying_top_of_stack = gen_primop_e(builder, get_stack_base_op, empty(a), empty(a));
                const Type* varying_raw_ptr_t = ptr_type(a, (PtrType) { .address_space = AsPrivatePhysical, .pointed_type = local_arr_ty });
                const Node* varying_raw_ptr = gen_reinterpret_cast(builder, varying_raw_ptr_t, varying_top_of_stack);
                const Type* varying_typed_ptr_t = ptr_type(a, (PtrType) { .address_space = AsPrivatePhysical, .pointed_type = element_type });
                const Node* varying_typed_ptr = gen_reinterpret_cast(builder, varying_typed_ptr_t, varying_top_of_stack);

                gen_store(builder, varying_typed_ptr, varying_value);
                for (int32_t j = 0; j < bytes_to_words_static(a, layout.size_in_bytes); j++) {
                    const Node* varying_logical_addr = gen_lea(builder, varying_raw_ptr, int32_literal(a, 0), nodes(a, 1, (const Node* []) {int32_literal(a, j) }));
                    const Node* input = gen_load(builder, varying_logical_addr);

                    const Node* partial_result = gen_primop_ce(builder, subgroup_broadcast_first_op, 1, (const Node* []) { input });

                    if (ctx->config->printf_trace.subgroup_ops)
                        gen_primop(builder, debug_printf_op, empty(a), mk_nodes(a, string_lit(a, (StringLiteral) { .string = "partial_result %d"}), partial_result));

                    gen_store(builder, varying_logical_addr, partial_result);
                }
                const Node* result = gen_load(builder, varying_typed_ptr);
                result = first(gen_primop(builder, subgroup_assume_uniform_op, empty(a), singleton(result)));
                return finish_body(builder, let(a, quote_helper(a, singleton(result)), tail));
            }
            default: break;
        }
    }

    return let(a, rewrite_node(&ctx->rewriter, old_instruction), tail);
}

static const Node* process(Context* ctx, const Node* node) {
    if (!node) return NULL;
    const Node* found = search_processed(&ctx->rewriter, node);
    if (found) return found;

    switch (node->tag) {
        case Let_TAG: return process_let(ctx, node);
        default: return recreate_node_identity(&ctx->rewriter, node);
    }
}

Module* lower_subgroup_ops(const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = get_arena_config(get_module_arena(src));
    IrArena* a = new_ir_arena(aconfig);
    Module* dst = new_module(a, get_module_name(src));
    assert(!config->lower.emulate_subgroup_ops && "TODO");
    Context ctx = {
        .rewriter = create_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config,
    };
    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    return dst;
}
