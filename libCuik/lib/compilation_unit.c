#include "compilation_unit.h"

CUIK_API void cuik_create_compilation_unit(CompilationUnit* restrict cu) {
    *cu = (CompilationUnit){0};
    cu->lock = HEAP_ALLOC(sizeof(mtx_t));
    mtx_init((mtx_t*) cu->lock, mtx_plain);
}

CUIK_API void cuik_lock_compilation_unit(CompilationUnit* restrict cu) {
    mtx_lock((mtx_t*) cu->lock);
}

CUIK_API void cuik_unlock_compilation_unit(CompilationUnit* restrict cu) {
    mtx_unlock((mtx_t*) cu->lock);
}

CUIK_API void cuik_add_to_compilation_unit(CompilationUnit* restrict cu, TranslationUnit* restrict tu) {
    assert(tu->next == NULL && "somehow the TU is already attached to something...");
    cuik_lock_compilation_unit(cu);

    tu->parent = cu;

    if (cu->tail == NULL) cu->head = tu;
    else cu->tail->next = tu;
    cu->tail = tu;
    cu->count += 1;

    cuik_unlock_compilation_unit(cu);
}

CUIK_API void cuik_destroy_compilation_unit(CompilationUnit* restrict cu) {
    // walk all the TUs and free them (if they're not freed already)
    TranslationUnit* tu = cu->head;
    while (tu != NULL) {
        TranslationUnit* next = tu->next;
        cuik_destroy_translation_unit(tu);
        tu = next;
    }

    mtx_destroy((mtx_t*) cu->lock);
    HEAP_FREE(cu->lock);
    *cu = (CompilationUnit){0};
}

CUIK_API size_t cuik_num_of_translation_units_in_compilation_unit(CompilationUnit* restrict cu) {
    return cu->count;
}

CUIK_API void cuik_internal_link_compilation_unit(CompilationUnit* restrict cu) {
    FOR_EACH_TU(tu, cu) {
        size_t count = dyn_array_length(tu->top_level_stmts);
        // printf("%s:\n", cuikpp_get_main_file(&tu->tokens));

        for (size_t i = 0; i < count; i++) {
            Stmt* s = tu->top_level_stmts[i];

            if (s->op == STMT_FUNC_DECL) {
                if (!s->decl.attrs.is_static &&
                    !s->decl.attrs.is_inline) {
                    // printf("  Func Export! %s\n", s->decl.name);
                    nl_strmap_put_cstr(cu->export_table, s->decl.name, s);
                }
            } else if (s->op == STMT_GLOBAL_DECL || s->op == STMT_DECL) {
                if (!s->decl.attrs.is_static &&
                    !s->decl.attrs.is_extern &&
                    !s->decl.attrs.is_typedef &&
                    !s->decl.attrs.is_inline &&
                    s->decl.type->kind != KIND_FUNC &&
                    s->decl.name != NULL) {
                    // printf("  Global Export! %s\n", s->decl.name);
                    nl_strmap_put_cstr(cu->export_table, s->decl.name, s);
                }
            }
        }
    }
}
