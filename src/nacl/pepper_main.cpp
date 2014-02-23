#include <cstdio>
#include <string>
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/var.h"
#include "z3.h"

class Z3Instance : public pp::Instance {

public:
	explicit Z3Instance(PP_Instance instance) : pp::Instance(instance) {}
	virtual ~Z3Instance() {}

	Z3_context ctx;
	Z3_solver solver;
	void setUpContext() {
		Z3_config cfg = Z3_mk_config();
		Z3_set_param_value(cfg, "MODEL", "true");
		ctx = Z3_mk_context(cfg);
		Z3_del_config(cfg);
		solver = Z3_mk_solver(ctx);
		Z3_solver_inc_ref(ctx, solver);
	}

	virtual void HandleMessage(const pp::Var& var_message) {
		pp::Var ret_value;
		if (!var_message.is_string()) {
			PostMessage(pp::Var("{error: 'message not a a string'}"));
			return;
		}
		if (!ctx) {
			PostMessage(pp::Var("{info: 'no context, setting up'}"));
			setUpContext();
			PostMessage(pp::Var("{info: 'context setup complete'"));
		}
		
		const Z3_symbol sort_names[] = {0};
		const Z3_sort sorts[] = {0};
		const Z3_symbol decl_names[] = {0};
		const Z3_func_decl decls[] = {0};
		PostMessage(pp::Var("{info: 'about to parse this'}"));
		Z3_solver_reset(ctx, solver);
		PostMessage(pp::Var("{info: 'reset successful'}"));
		const char* smtstring = var_message.AsString().c_str();
		Z3_ast result = Z3_parse_smtlib2_string(ctx, smtstring, 0, sort_names, sorts, 0, decl_names, decls);
		Z3_inc_ref(ctx, result);
		PostMessage(pp::Var("{info: 'smtlib2 string parsed'}"));
		Z3_error_code errcode = Z3_get_error_code(ctx);
		if (errcode != 0) {
			PostMessage(pp::Var("{error: 'Z3 threw an error during parsing SMTLIB2 string'}"));
			std::string s(Z3_get_error_msg_ex(ctx, errcode));
			s.insert(0, "{error: '");
			s.append("}");
			PostMessage(pp::Var(s));
			return;
		}
		PostMessage(pp::Var("{info: 'smtlib2 string parsing had no error'}"));
		Z3_solver_assert(ctx, solver, result);
		PostMessage(pp::Var("{info: 'assert ran'}"));
		Z3_lbool solveresult = Z3_solver_check(ctx, solver);
		PostMessage(pp::Var("{info: 'check ran'}"));
		if (solveresult) {
			Z3_model model = Z3_solver_get_model(ctx, solver);
			Z3_model_inc_ref(ctx, model);
			PostMessage(pp::Var("{info: 'get model ran'}"));
			std::string s(Z3_model_to_string(ctx, model));
			s.insert(0, "{result: '");
			s.append("'}");
			ret_value = pp::Var(s);
			Z3_model_dec_ref(ctx, model);
		} else if (solveresult == 0) {
			ret_value = pp::Var("{error: 'Z3 cannot solve this system'}");
		} else {
			ret_value = pp::Var("{error: 'The constraint system is unsatisfiable'}");
		}
		Z3_dec_ref(ctx, result);
		Z3_solver_reset(ctx, solver);

		PostMessage(ret_value);
	}
};

class Z3Module : public pp::Module {
public:
	Z3Module() : pp::Module() {}
	virtual ~Z3Module() {}

	virtual pp::Instance* CreateInstance(PP_Instance instance) {
		return new Z3Instance(instance);
	}
};

namespace pp {
	Module* CreateModule() {
		return new Z3Module();
	}
}
