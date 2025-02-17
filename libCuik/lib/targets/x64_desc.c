#include "targets.h"
#include <front/sema.h>

// two simple temporary buffers to represent type_as_string results
static thread_local char temp_string0[1024], temp_string1[1024];

static void set_defines(Cuik_CPP* cpp, Cuik_System sys) {
    target_generic_set_defines(cpp, sys, true, true);

    if (sys == CUIK_SYSTEM_WINDOWS) {
        cuikpp_define(cpp, "_M_X64", "100");
        cuikpp_define(cpp, "_AMD64_", "100");
        cuikpp_define(cpp, "_M_AMD64", "100");
    } else if (sys == CUIK_SYSTEM_LINUX) {
        cuikpp_define(cpp, "__x86_64__", "1");
        cuikpp_define(cpp, "__amd64",    "1");
        cuikpp_define(cpp, "__amd64__",  "1");
    }
}

// TODO(NeGate): Add some type checking utilities to match against a list of types since that's kinda important :p
static Cuik_Type* type_check_builtin(TranslationUnit* tu, Expr* e, const char* name, int arg_count, Expr** args) {
    Cuik_Type* t = target_generic_type_check_builtin(tu, e, name, arg_count, args);
    if (t != NULL) {
        return t;
    }

    if (strcmp(name, "_mm_setcsr") == 0) {
        if (arg_count != 1) {
            REPORT_EXPR(ERROR, e, "%s requires 1 arguments", name);
            return &builtin_types[TYPE_VOID];
        }

        Cuik_Type* arg_type = sema_expr(tu, args[0]);
        Cuik_Type* int_type = &builtin_types[TYPE_UINT];
        if (!type_compatible(tu, arg_type, int_type, args[0])) {
            type_as_string(tu, sizeof(temp_string0), temp_string0, arg_type);
            type_as_string(tu, sizeof(temp_string1), temp_string1, int_type);

            REPORT_EXPR(ERROR, args[0], "Could not implicitly convert type %s into %s.", temp_string0, temp_string1);
            return &builtin_types[TYPE_VOID];
        }

        args[0]->cast_type = &builtin_types[TYPE_UINT];
        return &builtin_types[TYPE_VOID];
    } else if (strcmp(name, "_mm_getcsr") == 0) {
        if (arg_count != 0) {
            REPORT_EXPR(ERROR, e, "%s requires 0 arguments", name);
        }

        return &builtin_types[TYPE_UINT];
    } else {
        REPORT_EXPR(ERROR, e->call.target, "unimplemented builtin '%s'", name);
        return &builtin_types[TYPE_VOID];
    }
}

#ifdef CUIK_USE_TB
// on Win64 all structs that have a size of 1,2,4,8
// or any scalars are passed via registers
static bool win64_should_pass_via_reg(TranslationUnit* tu, Cuik_Type* type) {
    if (type->kind == KIND_STRUCT || type->kind == KIND_UNION) {
        switch (type->size) {
            case 1:
            case 2:
            case 4:
            case 8:
            return true;
            default:
            return false;
        }
    } else {
        return true;
    }
}

static TB_FunctionPrototype* create_prototype(TranslationUnit* tu, Cuik_Type* type) {
    // decide if return value is aggregate
    TB_Module* m = tu->ir_mod;
    bool is_aggregate_return = !win64_should_pass_via_reg(tu, type->func.return_type);

    // parameters
    Param* param_list = type->func.param_list;
    size_t param_count = type->func.param_count;

    // estimate parameter count
    size_t real_param_count = (is_aggregate_return ? 1 : 0) + param_count;

    TB_DataType return_dt = TB_TYPE_PTR;
    if (!is_aggregate_return) return_dt = ctype_to_tbtype(type->func.return_type);

    TB_FunctionPrototype* proto = tb_prototype_create(tu->ir_mod, TB_STDCALL, return_dt, real_param_count, type->func.has_varargs);

    if (is_aggregate_return) {
        if (tu->has_tb_debug_info) {
            // it's a pointer to the debug type here
            TB_DebugType* dbg_type = cuik__as_tb_debug_type(m, type->func.return_type);
            tb_prototype_add_param_named(proto, TB_TYPE_PTR, "$retval", tb_debug_create_ptr(m, dbg_type));
        } else {
            tb_prototype_add_param(proto, TB_TYPE_PTR);
        }
    }

    for (size_t i = 0; i < param_count; i++) {
        Param* p = &param_list[i];

        if (win64_should_pass_via_reg(tu, p->type)) {
            TB_DataType dt = ctype_to_tbtype(p->type);

            assert(dt.width < 8);
            if (tu->has_tb_debug_info) {
                tb_prototype_add_param_named(proto, dt, p->name, cuik__as_tb_debug_type(tu->ir_mod, p->type));
            } else {
                tb_prototype_add_param(proto, dt);
            }
        } else {
            if (tu->has_tb_debug_info) {
                TB_DebugType* dbg_type = cuik__as_tb_debug_type(tu->ir_mod, p->type);
                tb_prototype_add_param_named(proto, TB_TYPE_PTR, p->name, tb_debug_create_ptr(m, dbg_type));
            } else {
                tb_prototype_add_param(proto, TB_TYPE_PTR);
            }
        }
    }

    return proto;
}

static bool pass_return_via_reg(TranslationUnit* tu, Cuik_Type* type) {
    return win64_should_pass_via_reg(tu, type);
}

static int deduce_parameter_usage(TranslationUnit* tu, Cuik_Type* type) {
    return 1;
}

static int pass_parameter(TranslationUnit* tu, TB_Function* func, Expr* e, bool is_vararg, TB_Reg* out_param) {
    Cuik_Type* arg_type = e->type;

    if (!win64_should_pass_via_reg(tu, arg_type)) {
        // const pass-by-value is considered as a const ref
        // since it doesn't mutate
        IRVal arg = irgen_expr(tu, func, e);
        TB_Reg arg_addr = TB_NULL_REG;
        switch (arg.value_type) {
            case LVALUE:
            arg_addr = arg.reg;
            break;
            case LVALUE_SYMBOL:
            arg_addr = tb_inst_get_symbol_address(func, arg.sym);
            break;
            case RVALUE: {
                // spawn a lil temporary
                TB_CharUnits size = arg_type->size;
                TB_CharUnits align = arg_type->align;
                TB_DataType dt = tb_function_get_node(func, arg.reg)->dt;

                arg_addr = tb_inst_local(func, size, align);
                tb_inst_store(func, dt, arg_addr, arg.reg, align);
                break;
            }
            default:
            break;
        }
        assert(arg_addr);

        // TODO(NeGate): we might wanna define some TB instruction
        // for killing locals since some have really limited lifetimes
        TB_CharUnits size = arg_type->size;
        TB_CharUnits align = arg_type->align;

        if (arg_type->is_const) {
            out_param[0] = arg_addr;
        } else {
            TB_Reg temp_slot = tb_inst_local(func, size, align);
            TB_Reg size_reg = tb_inst_uint(func, TB_TYPE_I64, size);

            tb_inst_memcpy(func, temp_slot, arg_addr, size_reg, align);

            out_param[0] = temp_slot;
        }

        return 1;
    } else {
        if (arg_type->kind == KIND_STRUCT ||
            arg_type->kind == KIND_UNION) {
            // Convert aggregate into TB scalar
            IRVal arg = irgen_expr(tu, func, e);
            TB_Reg arg_addr = TB_NULL_REG;
            switch (arg.value_type) {
                case LVALUE:
                arg_addr = arg.reg;
                break;
                case LVALUE_SYMBOL:
                arg_addr = tb_inst_get_symbol_address(func, arg.sym);
                break;
                case RVALUE: {
                    // spawn a lil temporary
                    TB_CharUnits size = arg_type->size;
                    TB_CharUnits align = arg_type->align;
                    TB_DataType dt = tb_function_get_node(func, arg.reg)->dt;

                    arg_addr = tb_inst_local(func, size, align);
                    tb_inst_store(func, dt, arg_addr, arg.reg, align);
                    break;
                }
                default:
                break;
            }
            assert(arg_addr);

            TB_DataType dt = TB_TYPE_VOID;
            switch (arg_type->size) {
                case 1:
                dt = TB_TYPE_I8;
                break;
                case 2:
                dt = TB_TYPE_I16;
                break;
                case 4:
                dt = TB_TYPE_I32;
                break;
                case 8:
                dt = TB_TYPE_I64;
                break;
                default:
                break;
            }

            out_param[0] = tb_inst_load(func, dt, arg_addr, arg_type->align);
            return 1;
        } else {
            TB_Reg arg = irgen_as_rvalue(tu, func, e);
            TB_DataType dt = tb_function_get_node(func, arg)->dt;

            if (is_vararg && dt.type == TB_FLOAT && dt.data == TB_FLT_64 && dt.width == 0) {
                // convert any float variadic arguments into integers
                arg = tb_inst_bitcast(func, arg, TB_TYPE_I64);
            }

            out_param[0] = arg;
            return 1;
        }
    }
}

static TB_Reg compile_builtin(TranslationUnit* tu, TB_Function* func, const char* name, int arg_count, Expr** args) {
    BuiltinResult r = target_generic_compile_builtin(tu, func, name, arg_count, args);
    if (!r.failure) {
        return r.r;
    }

    // x64 specific builtins
    if (strcmp(name, "_mm_setcsr") == 0) {
        return tb_inst_x86_ldmxcsr(func, irgen_as_rvalue(tu, func, args[0]));
    } else if (strcmp(name, "_mm_getcsr") == 0) {
        return tb_inst_x86_stmxcsr(func);
    }

    internal_error("unimplemented builtin! %s", name);
    return 0;
}
#endif /* CUIK_USE_TB */

const Cuik_ArchDesc* cuik_get_x64_target_desc(void) {
    static Cuik_ArchDesc t = { 0 };
    if (t.builtin_func_map == NULL) {
        // TODO(NeGate): make this thread safe
        NL_Strmap(bool) builtins = NULL;

        target_generic_fill_builtin_table(&builtins);
        nl_strmap_put_cstr(builtins, "_mm_getcsr", 1);
        nl_strmap_put_cstr(builtins, "_mm_setcsr", 1);

        t = (Cuik_ArchDesc){
            .arch = TB_ARCH_X86_64,

            .builtin_func_map = builtins,
            .set_defines = set_defines,
            #ifdef CUIK_USE_TB
            .create_prototype = create_prototype,
            .pass_return_via_reg = pass_return_via_reg,
            .deduce_parameter_usage = deduce_parameter_usage,
            .pass_parameter = pass_parameter,
            .compile_builtin = compile_builtin,
            #endif /* CUIK_USE_TB */
            .type_check_builtin = type_check_builtin,
        };
    }

    return &t;
}
