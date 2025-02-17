#include <cuik.h>
#include <cuik_ast.h>
#include <stdio.h>
#include "helper.h"

static void dump_tokens(FILE* out_file, TokenStream* s) {
    const char* last_file = NULL;
    int last_line = 0;

    Token* tokens = cuik_get_tokens(s);
    size_t count = cuik_get_token_count(s);

    for (size_t i = 0; i < count; i++) {
        Token* t = &tokens[i];
        SourceLoc* loc = &s->locations[SOURCE_LOC_GET_DATA(t->location)];

        if (last_file != loc->line->filepath && strcmp(loc->line->filepath, "<temp>") != 0) {
            char str[MAX_PATH];

            // TODO(NeGate): Kinda shitty but i just wanna duplicate
            // the backslashes to avoid them being treated as an escape
            const char* in = (const char*)loc->line->filepath;
            char* out = str;

            while (*in) {
                if (*in == '\\') {
                    *out++ = '\\';
                    *out++ = '\\';
                    in++;
                } else {
                    *out++ = *in++;
                }
            }
            *out++ = '\0';

            fprintf(out_file, "\n#line %d \"%s\"\t", loc->line->line, str);
            last_file = loc->line->filepath;
        }

        if (last_line != loc->line->line) {
            fprintf(out_file, "\n/* line %3d */\t", loc->line->line);
            last_line = loc->line->line;
        }

        fprintf(out_file, "%.*s ", (int)(t->end - t->start), t->start);
    }
}

static void da_visitor(TranslationUnit* tu, Stmt* restrict s, void* user_data) {
    if (!cuik_is_in_main_file(tu, s->loc)) return;

    char* name = (char*)s->decl.name;
    if (s->op == STMT_FUNC_DECL || s->op == STMT_GLOBAL_DECL || s->op == STMT_DECL) {
        printf("func %s(", name);
        printf(");\n");
    }
}

int main(int argc, char** argv) {
    if (argc <= 1) {
        fprintf(stderr, "no input file!\n");
        return 1;
    }

    cuik_init();

    // find system libraries
    Cuik_SystemLibs* system_libs;
    {
        char crt_dir[FILENAME_MAX];
        if (!get_exe_path(crt_dir)) {
            fprintf(stderr, "error: could not locate executable path");
            return 1;
        }

        char* slash = (char*)step_out_dir(crt_dir, 2);
        if (slash == NULL) {
            fprintf(stderr, "error: could not locate executable path");
            return 1;
        }

        *slash = '\0';
        system_libs = cuik_get_system_includes(crt_dir);
    }

    bool dump_ast = false;
    TB_Module* mod = NULL;
    if (!dump_ast) {
        TB_FeatureSet features = {0};
        mod = tb_module_create(TB_ARCH_X86_64, TB_SYSTEM_WINDOWS, TB_DEBUGFMT_NONE, &features);
    }

    // preproc
    Cuik_CPP cpp;
    TokenStream tokens = cuik_preprocess_simple(&cpp, argv[1], system_libs, 2, (const char*[]) {
            "include/",
            "src/"
        });

    #if 0
    FILE* out = fopen("./a.i", "wb");
    assert(out != NULL);
    dump_tokens(out, &tokens);
    fclose(out);
    #else
    // parse
    TranslationUnit* tu = cuik_parse_translation_unit(mod, &tokens, NULL);

    // print defines
    CUIKPP_FOR_DEFINES(it, &cpp) {
        if (cuik_is_in_main_file(tu, it.loc)) {
            printf("#define %.*s %.*s\n",
                (int)it.key.len, it.key.data,
                (int)it.value.len, it.value.data
            );
        }
    }

    cuikpp_finalize(&cpp);

    cuik_visit_top_level(tu, NULL, da_visitor);
    cuik_destroy_translation_unit(tu);
    #endif

    cuikpp_deinit(&cpp);
    return 0;
}
