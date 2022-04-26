#include "passes.h"

#include "list.h"

#include "../log.h"
#include "../local_array.h"

#include <assert.h>
#include <string.h>

struct BindEntry {
    const char* name;
    const Node* bound_node;
};

struct BindRewriter {
    Rewriter rewriter;
    struct List* bound_variables;
    const Node* current_function;
};

static const Node* resolve(struct BindRewriter* ctx, const char* name) {
    for (size_t i = 0; i < entries_count_list(ctx->bound_variables); i++) {
        const struct BindEntry* entry = &read_list(const struct BindEntry, ctx->bound_variables)[i];
        if (strcmp(entry->name, name) == 0) {
            return entry->bound_node;
        }
    }
    error("could not resolve variable %s", name)
}

static const Node* bind_node(struct BindRewriter* ctx, const Node* node);

static Node* rewrite_fn_head(struct BindRewriter* ctx, const Node* node) {
    assert(node != NULL && node->tag == Function_TAG);
    Rewriter* rewriter = &ctx->rewriter;
    IrArena* dst_arena = rewriter->dst_arena;

    // rebuild the parameters and shove them in the list
    size_t params_count = node->payload.fn.params.count;
    LARRAY(const Node*, nparams, params_count);
    for (size_t i = 0; i < params_count; i++) {
        const Variable* old_param = &node->payload.fn.params.nodes[i]->payload.var;
        const Node* new_param = var(dst_arena, rewrite_node(rewriter, old_param->type), string(dst_arena, old_param->name));
        nparams[i] = new_param;
    }

    return fn(dst_arena, node->payload.fn.atttributes, string(dst_arena, node->payload.fn.name), nodes(dst_arena, params_count, nparams), rewrite_nodes(rewriter, node->payload.fn.return_types));
}

static void rewrite_fn_body(struct BindRewriter* ctx, const Node* node, Node* target) {
    assert(node != NULL && node->tag == Function_TAG);
    Rewriter* rewriter = &ctx->rewriter;
    IrArena* dst_arena = rewriter->dst_arena;

    size_t old_bound_variables_size = entries_count_list(ctx->bound_variables);

    // bind the rebuilt parameters for rewriting the body
    for (size_t i = 0; i < node->payload.fn.params.count; i++) {
        const Node* param = target->payload.fn.params.nodes[i];
        struct BindEntry entry = {
            .name = string(dst_arena, param->payload.var.name),
            .bound_node = param
        };
        append_list(struct BindEntry, ctx->bound_variables, entry);
        printf("Bound param %s\n", entry.name);
    }

    struct BindRewriter sub_ctx = *ctx;
    if (!node->payload.fn.atttributes.is_continuation) {
        assert(ctx->current_function == NULL);
        sub_ctx.current_function = target;
    } else {
        // maybe not beneficial/relevant
        assert(sub_ctx.current_function != NULL);
    }

    target->payload.fn.block = bind_node(&sub_ctx, node->payload.fn.block);

    // cleanup
    while (entries_count_list(ctx->bound_variables) > old_bound_variables_size)
        remove_last_list(struct BindEntry, ctx->bound_variables);
}

static const Node* bind_node(struct BindRewriter* ctx, const Node* node) {
    if (node == NULL)
        return NULL;

    Rewriter* rewriter = &ctx->rewriter;
    IrArena* dst_arena = rewriter->dst_arena;
    switch (node->tag) {
        case Root_TAG: {
            const Root* src_root = &node->payload.root;
            const size_t count = src_root->declarations.count;

            LARRAY(const Node*, new_decls, count);

            for (size_t i = 0; i < count; i++) {
                const Node* decl = src_root->declarations.nodes[i];

                const Node* bound = NULL;
                struct BindEntry entry;

                switch (decl->tag) {
                    case Variable_TAG: {
                        const Variable* ovar = &decl->payload.var;
                        bound = var(rewriter->dst_arena, rewrite_node(rewriter, ovar->type), string(rewriter->dst_arena, ovar->name));
                        entry.name = ovar->name;
                        break;
                    }
                    case Constant_TAG: {
                        const Constant* cnst = &decl->payload.constant;
                        Node* new_constant = constant(dst_arena, cnst->name);
                        new_constant->payload.constant.type_hint = decl->payload.constant.type_hint;
                        bound = new_constant;
                        entry.name = cnst->name;
                        break;
                    }
                    case Function_TAG: {
                        const Function* ofn = &decl->payload.fn;
                        bound = rewrite_fn_head(ctx, decl);
                        entry.name = ofn->name;
                        break;
                    }
                    default: error("unknown declaration kind");
                }

                entry.bound_node = bound;
                append_list(struct BindEntry, ctx->bound_variables, entry);
                printf("Bound root def %s\n", entry.name);

                new_decls[i] = bound;
            }

            for (size_t i = 0; i < count; i++) {
                const Node* odecl = src_root->declarations.nodes[i];
                if (odecl->tag != Variable_TAG)
                new_decls[i] = bind_node(ctx, odecl);
            }

            return root(rewriter->dst_arena, (Root) {
                .declarations = nodes(rewriter->dst_arena, count, new_decls),
            });
        }
        case Variable_TAG: error("the binders should be handled such that this node is never reached");
        case Unbound_TAG: {
            return resolve(ctx, node->payload.unbound.name);
        }
        case Let_TAG: {
            const Node* bound_instr = bind_node(ctx, node->payload.let.instruction);

            size_t outputs_count = node->payload.let.variables.count;
            LARRAY(const Node*, noutputs, outputs_count);
            for (size_t p = 0; p < outputs_count; p++) {
                const Variable* old_var = &node->payload.let.variables.nodes[p]->payload.var;
                const Node* new_binding = var(rewriter->dst_arena, rewrite_node(rewriter, old_var->type), string(rewriter->dst_arena, old_var->name));
                noutputs[p] = new_binding;
                struct BindEntry entry = {
                    .name = string(ctx->rewriter.dst_arena, old_var->name),
                    .bound_node = new_binding
                };
                append_list(struct BindEntry, ctx->bound_variables, entry);
                printf("Bound primop result %s\n", entry.name);
            }

            return let(rewriter->dst_arena, (Let) {
                .variables = nodes(rewriter->dst_arena, outputs_count, noutputs),
                .instruction = bound_instr,
            });
        }
        case ParsedBlock_TAG: {
            const ParsedBlock* pblock = &node->payload.parsed_block;
            size_t old_bound_variables_size = entries_count_list(ctx->bound_variables);

            size_t inner_conts_count = pblock->continuations_vars.count;
            LARRAY(Node*, new_conts, inner_conts_count);

            // First create stubs and inline that crap
            for (size_t i = 0; i < inner_conts_count; i++) {
                Node* new_cont = rewrite_fn_head(ctx, pblock->continuations.nodes[i]);
                new_conts[i] = new_cont;
                struct BindEntry entry = {
                    .name = string(ctx->rewriter.dst_arena, pblock->continuations_vars.nodes[i]->payload.var.name),
                    .bound_node = new_cont
                };
                append_list(struct BindEntry, ctx->bound_variables, entry);
                printf("Bound (stub) continuation %s\n", entry.name);
            }

            const Node* new_block = block(rewriter->dst_arena, (Block) {
                .instructions = rewrite_nodes(rewriter, pblock->instructions),
                .terminator = bind_node(ctx, pblock->terminator)
            });

            // Rebuild the actual continuations now
            for (size_t i = 0; i < inner_conts_count; i++) {
                rewrite_fn_body(ctx, pblock->continuations.nodes[i], new_conts[i]);
                printf("Processed (full) continuation %s\n", new_conts[i]->payload.fn.name);
            }

            while (entries_count_list(ctx->bound_variables) > old_bound_variables_size)
                remove_last_list(struct BindEntry, ctx->bound_variables);

            return new_block;
        }
        case Block_TAG: {
            return block(rewriter->dst_arena, (Block) {
                .instructions = rewrite_nodes(rewriter, node->payload.block.instructions),
            });
        }
        case Return_TAG: {
            assert(ctx->current_function);
            return fn_ret(dst_arena, (Return) {
                .fn = ctx->current_function,
                .values = rewrite_nodes(rewriter, node->payload.fn_ret.values)
            });
        }
        case Function_TAG: {
            Node* head = (Node*) resolve(ctx, node->payload.fn.name);
            rewrite_fn_body(ctx, node, head);
            return head;
        }
        case Constant_TAG: {
            Node* head = (Node*) resolve(ctx, node->payload.fn.name);
            head->payload.constant.value = bind_node(ctx, node->payload.constant.value);
            return head;
        }
        default: return recreate_node_identity(&ctx->rewriter, node);
    }
}

const Node* bind_program(IrArena* src_arena, IrArena* dst_arena, const Node* source) {
    struct List* bound_variables = new_list(struct BindEntry);
    struct BindRewriter ctx = {
        .rewriter = {
            .src_arena = src_arena,
            .dst_arena = dst_arena,
            .rewrite_fn = (RewriteFn) bind_node,
        },
        .bound_variables = bound_variables
    };

    const Node* rewritten = rewrite_node(&ctx.rewriter, source);

    destroy_list(bound_variables);
    return rewritten;
}