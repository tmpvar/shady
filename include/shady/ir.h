#ifndef SHADY_IR_H
#define SHADY_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct IrArena_ IrArena;
typedef struct Node_ Node;
typedef struct Node_ Type;
typedef unsigned int VarId;
typedef const char* String;

//////////////////////////////// Lists & Strings ////////////////////////////////

typedef struct Nodes_ {
    size_t count;
    const Node** nodes;
} Nodes;

typedef struct Strings_ {
    size_t count;
    String* strings;
} Strings;

Nodes     nodes(IrArena*, size_t count, const Node*[]);
Strings strings(IrArena*, size_t count, const char*[]);

#define empty(arena) nodes(arena, 0, NULL)
Nodes singleton(const Node* type);
#define mk_nodes(arena, ...) nodes(arena, sizeof((const Node*[]) { __VA_ARGS__ }) / sizeof(const Node*), (const Node*[]) { __VA_ARGS__ })

const Node* first(Nodes nodes);

Nodes append_nodes(IrArena*, Nodes, const Node*);
Nodes concat_nodes(IrArena*, Nodes, Nodes);

String string_sized(IrArena* arena, size_t size, const char* start);
String string(IrArena* arena, const char* start);
String format_string(IrArena* arena, const char* str, ...);
String unique_name(IrArena* arena, const char* start);
String name_type_safe(IrArena*, const Type*);

//////////////////////////////// IR Arena ////////////////////////////////

typedef struct {
    bool name_bound;
    bool check_types;
    bool allow_fold;
    /// Selects which type the subgroup intrinsic primops use to manipulate masks
    enum {
        /// Uses the MaskType
        SubgroupMaskAbstract,
        /// Uses four packed 32-bit integers
        SubgroupMaskSpvKHRBallot
    } subgroup_mask_representation;
} ArenaConfig;

IrArena* new_ir_arena(ArenaConfig);
void destroy_ir_arena(IrArena*);

//////////////////////////////// Modules ////////////////////////////////

typedef struct Module_ Module;

Module* new_module(IrArena*, String name);

IrArena* get_module_arena(const Module*);
String get_module_name(const Module*);
Nodes get_module_declarations(const Module*);

//////////////////////////////// Grammar ////////////////////////////////

// The language grammar is big enough that it deserve its own files

#include "primops.h"
#include "grammar.h"

//////////////////////////////// Getters ////////////////////////////////

/// Get the name out of a global variable, function or constant
String get_decl_name(const Node*);

const IntLiteral* resolve_to_literal(const Node*);

int64_t extract_int_literal_value(const Node*, bool sign_extend);
const char* extract_string_literal(IrArena*, const Node*);

static inline bool is_physical_as(AddressSpace as) { return as <= AsGlobalLogical; }

/// Returns true if variables in that address space can contain different data for threads in the same subgroup
bool is_addr_space_uniform(AddressSpace);

const Node* lookup_annotation(const Node* decl, const char* name);
const Node* extract_annotation_value(const Node* annotation);
Nodes extract_annotation_values(const Node* annotation);
/// Gets the string literal attached to an annotation, if present.
const char* extract_annotation_string_payload(const Node* annotation);
bool lookup_annotation_with_string_payload(const Node* decl, const char* annotation_name, const char* expected_payload);
bool is_annotation(const Node* node);
String get_annotation_name(const Node* node);

String get_abstraction_name(const Node* abs);
const Node* get_abstraction_body(const Node*);
Nodes get_abstraction_params(const Node*);
Module* get_abstraction_module(const Node*);

const Node* get_let_instruction(const Node* let);
const Node* get_let_tail(const Node* let);

//////////////////////////////// Constructors ////////////////////////////////

// autogenerated node ctors
#define NODE_CTOR_DECL_1(struct_name, short_name) const Node* short_name(IrArena*, struct_name);
#define NODE_CTOR_DECL_0(struct_name, short_name) const Node* short_name(IrArena*);
#define NODE_CTOR_1(has_payload, struct_name, short_name) NODE_CTOR_DECL_##has_payload(struct_name, short_name)
#define NODE_CTOR_0(has_payload, struct_name, short_name)
#define NODE_CTOR(autogen_ctor, _, has_payload, struct_name, short_name) NODE_CTOR_##autogen_ctor(has_payload, struct_name, short_name)
NODES(NODE_CTOR)
#undef NODE_CTOR
#undef NODE_CTOR_0
#undef NODE_CTOR_1
#undef NODE_CTOR_DECL_0
#undef NODE_CTOR_DECL_1

const Node* var(IrArena* arena, const Type* type, const char* name);

const Node* tuple(IrArena* arena, Nodes contents);

Node* function    (Module*, Nodes params, const char* name, Nodes annotations, Nodes return_types);
Node* constant    (Module*, Nodes annotations, const Type*, const char* name);
Node* global_var  (Module*, Nodes annotations, const Type*, String, AddressSpace);
Type* nominal_type(Module*, Nodes annotations, String name);

Node* basic_block (IrArena*, Node* function, Nodes params, const char* name);
Node* lambda      (Module*, Nodes params);

const Node* let(IrArena* arena, const Node* instruction, const Node* tail);
const Node* let_mut(IrArena* arena, const Node* instruction, const Node* tail);
const Node* let_into(IrArena* arena, const Node* instruction, const Node* tail);
const Node* let_indirect(IrArena* arena, const Node* instruction, const Node* tail);

typedef struct BodyBuilder_ BodyBuilder;
BodyBuilder* begin_body(Module*);

/// Appends an instruction to the builder, may apply optimisations.
/// If the arena is typed, returns a list of variables bound to the values yielded by that instruction
Nodes bind_instruction(BodyBuilder*, const Node* instruction);

/// Like append instruction, but you explicitly give it information about any yielded values
/// ! In untyped arenas, you need to call this because we can't guess how many things are returned without typing info !
Nodes bind_instruction_extra_mutable(BodyBuilder*, const Node* initial_value, size_t outputs_count, Nodes* provided_types, String const output_names[]);
Nodes bind_instruction_extra(BodyBuilder*, const Node* initial_value, size_t outputs_count, Nodes* provided_types, String const output_names[]);

const Node* finish_body(BodyBuilder* builder, const Node* terminator);

const Type* int8_type(IrArena* arena);
const Type* int16_type(IrArena* arena);
const Type* int32_type(IrArena* arena);
const Type* int64_type(IrArena* arena);

const Type* int8_literal(IrArena* arena,  int8_t i);
const Type* int16_literal(IrArena* arena, int16_t i);
const Type* int32_literal(IrArena* arena, int32_t i);
const Type* int64_literal(IrArena* arena, int64_t i);

const Type* uint8_literal(IrArena* arena,  uint8_t i);
const Type* uint16_literal(IrArena* arena, uint16_t i);
const Type* uint32_literal(IrArena* arena, uint32_t i);
const Type* uint64_literal(IrArena* arena, uint64_t i);

/// Turns a value into an 'instruction' (the enclosing let will be folded away later)
/// Useful for local rewrites
const Node* quote(IrArena* arena, const Node* value);
/// Produces a nothing value, same use as quote
const Node* unit(IrArena* arena);

//////////////////////////////// Compilation ////////////////////////////////

typedef struct CompilerConfig_ {
    bool allow_frontend_syntax;
    uint32_t per_thread_stack_size;
    uint32_t per_subgroup_stack_size;

    uint32_t subgroup_size;

    struct {
        uint8_t major;
        uint8_t minor;
    } target_spirv_version;

    struct {
        bool emulate_subgroup_ops;
        bool emulate_subgroup_ops_extended_types;
    } lower;

    struct {
        bool skip_generated, skip_builtin;
    } logging;
} CompilerConfig;

CompilerConfig default_compiler_config();

typedef enum CompilationResult_ {
    CompilationNoError
} CompilationResult;

CompilationResult parse_files(CompilerConfig*, size_t num_files, const char** files_contents, Module* module);
CompilationResult run_compiler_passes(CompilerConfig* config, Module** mod);

//////////////////////////////// Emission ////////////////////////////////

void emit_spirv(CompilerConfig* config, Module*, size_t* output_size, char** output);

typedef enum {
    C,
    GLSL
} CDialect;
void emit_c(CompilerConfig* config, CDialect, Module*, size_t* output_size, char** output);

void dump_cfg(FILE* file, Module*);
void dump_module(Module*);
void print_module_into_str(Module*, char** str_ptr, size_t*);
void dump_node(const Node* node);

#endif
