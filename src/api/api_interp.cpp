/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    api_interp.cpp

Abstract:
    API for interpolation

Author:

    Ken McMillan

Revision History:

--*/
#include<iostream>
#include<sstream>
#include<vector>
#include"z3.h"
#include"api_log_macros.h"
#include"api_context.h"
#include"api_tactic.h"
#include"api_solver.h"
#include"api_model.h"
#include"api_stats.h"
#include"api_ast_vector.h"
#include"tactic2solver.h"
#include"scoped_ctrl_c.h"
#include"cancel_eh.h"
#include"scoped_timer.h"
#include"smt_strategic_solver.h"
#include"smt_solver.h"
#include"smt_implied_equalities.h"
#include"iz3interp.h"
#include"iz3profiling.h"
#include"iz3hash.h"
#include"iz3pp.h"
#include"iz3checker.h"

using namespace stl_ext;

// WARNING: don't make a hash_map with this if the range type
// has a destructor: you'll get an address dependency!!!
namespace stl_ext {
  template <>
  class hash<Z3_ast> {
  public:
    size_t operator()(const Z3_ast p) const {
      return (size_t) p;
    }
  };
}

typedef interpolation_options_struct *Z3_interpolation_options;

extern "C" {

  Z3_context Z3_mk_interpolation_context(Z3_config cfg){
    if(!cfg) cfg = Z3_mk_config();
    Z3_set_param_value(cfg, "PROOF", "true");
    Z3_set_param_value(cfg, "MODEL", "true");
    // Z3_set_param_value(cfg, "PRE_SIMPLIFIER","false");
    // Z3_set_param_value(cfg, "SIMPLIFY_CLAUSES","false");
    
    Z3_context ctx = Z3_mk_context(cfg);
    Z3_del_config(cfg);
    return ctx;
  }
  
  void Z3_interpolate_proof(Z3_context ctx,
			    Z3_ast proof,
			    int num,
			    Z3_ast *cnsts,
			    unsigned *parents,
			    Z3_params options,
			    Z3_ast *interps,
			    int num_theory,
			    Z3_ast *theory
			    ){

      if(num > 1){ // if we have interpolants to compute
	
	ptr_vector<ast> pre_cnsts_vec(num);  // get constraints in a vector
	for(int i = 0; i < num; i++){
	  ast *a = to_ast(cnsts[i]);
	  pre_cnsts_vec[i] = a;
	}
	
	::vector<int> pre_parents_vec;  // get parents in a vector
	if(parents){
          pre_parents_vec.resize(num);
	  for(int i = 0; i < num; i++)
	    pre_parents_vec[i] = parents[i];
        }
	
	ptr_vector<ast> theory_vec; // get background theory in a vector
	if(theory){
	  theory_vec.resize(num_theory);
	  for(int i = 0; i < num_theory; i++)
	    theory_vec[i] = to_ast(theory[i]);
	}
	
	ptr_vector<ast> interpolants(num-1); // make space for result
	
	ast_manager &_m = mk_c(ctx)->m();
	iz3interpolate(_m,
		       to_ast(proof),
		       pre_cnsts_vec,
		       pre_parents_vec,
		       interpolants,
		       theory_vec,
		       0); // ignore params for now FIXME

	// copy result back
	for(unsigned i = 0; i < interpolants.size(); i++){
	  mk_c(ctx)->save_ast_trail(interpolants[i]);
	  interps[i] = of_ast(interpolants[i]);
	  _m.dec_ref(interpolants[i]);
	}
      }
  }

  Z3_lbool Z3_interpolate(Z3_context ctx,
			  int num,
			  Z3_ast *cnsts,
			  unsigned *parents,
			  Z3_params options,
			  Z3_ast *interps,
			  Z3_model *model,
			  Z3_literals *labels,
			  int incremental,
			  int num_theory,
                          Z3_ast *theory
			  ){

    
    profiling::timer_start("Solve");

    if(!incremental){

      profiling::timer_start("Z3 assert");

      Z3_push(ctx); // so we can rewind later
      
      for(int i = 0; i < num; i++)
	Z3_assert_cnstr(ctx,cnsts[i]);  // assert all the constraints

      if(theory){
	for(int i = 0; i < num_theory; i++)
	  Z3_assert_cnstr(ctx,theory[i]);
      }

      profiling::timer_stop("Z3 assert");
    }


    // Get a proof of unsat
      
    Z3_ast proof;
    Z3_lbool result;
    
    profiling::timer_start("Z3 solving");
    result = Z3_check_assumptions(ctx, 0, 0, model, &proof, 0, 0);
    profiling::timer_stop("Z3 solving");
    
    switch (result) {
    case Z3_L_FALSE:
      
      Z3_interpolate_proof(ctx,
			   proof,
			   num,
			   cnsts,
			   parents,
			   options,
			   interps,
			   num_theory,
			   theory);

      if(!incremental)
	for(int i = 0; i < num-1; i++)
	  Z3_persist_ast(ctx,interps[i],1);
      break;
      
    case Z3_L_UNDEF:
      if(labels)
	*labels = Z3_get_relevant_labels(ctx);
      break;

    case Z3_L_TRUE:
      if(labels)
	*labels = Z3_get_relevant_labels(ctx);
      break;
    }

    profiling::timer_start("Z3 pop");
    if(!incremental)
      Z3_pop(ctx,1);
    profiling::timer_stop("Z3 pop");

    profiling::timer_stop("Solve");

    return result;

  }
  
  static std::ostringstream itp_err;

  int Z3_check_interpolant(Z3_context ctx, 
			    int num, 
			    Z3_ast *cnsts, 
			    int *parents,
			    Z3_ast *itp,
			    const char **error,
			    int num_theory,
			    Z3_ast *theory){

    ast_manager &_m = mk_c(ctx)->m();
    itp_err.clear();
    
    // need a solver -- make one here, but how?
    params_ref p = params_ref::get_empty(); //FIXME
    scoped_ptr<solver_factory> sf(mk_smt_solver_factory());
    scoped_ptr<solver> sp((*(sf))(_m, p, false, true, false, symbol("AUFLIA")));
    
    ptr_vector<ast> cnsts_vec(num);  // get constraints in a vector
    for(int i = 0; i < num; i++){
      ast *a = to_ast(cnsts[i]);
      cnsts_vec[i] = a;
    }
    
    ptr_vector<ast> itp_vec(num);  // get interpolants in a vector
    for(int i = 0; i < num-1; i++){
      ast *a = to_ast(itp[i]);
      itp_vec[i] = a;
    }

    ::vector<int> parents_vec;  // get parents in a vector
    if(parents){
      parents_vec.resize(num);
      for(int i = 0; i < num; i++)
	parents_vec[i] = parents[i];
    }
    
    ptr_vector<ast> theory_vec; // get background theory in a vector
    if(theory){
      theory_vec.resize(num_theory);
      for(int i = 0; i < num_theory; i++)
	theory_vec[i] = to_ast(theory[i]);
    }
    
    bool res = iz3check(_m,
			sp.get(),
			itp_err,
			cnsts_vec,
			parents_vec,
			itp_vec,
			theory_vec);

    *error = res ? 0 : itp_err.str().c_str();
    return res;
  }


  static std::string Z3_profile_string;
  
  Z3_string Z3_interpolation_profile(Z3_context ctx){
    std::ostringstream f;
    profiling::print(f);
    Z3_profile_string = f.str();
    return Z3_profile_string.c_str();
  }


  Z3_interpolation_options
  Z3_mk_interpolation_options(){
    return (Z3_interpolation_options) new interpolation_options_struct;
  }

  void
  Z3_del_interpolation_options(Z3_interpolation_options opts){
    delete opts;
  }

  void
  Z3_set_interpolation_option(Z3_interpolation_options opts, 
			      Z3_string name,
			      Z3_string value){
    opts->map[name] = value;
  }



};


static void tokenize(const std::string &str, std::vector<std::string> &tokens){
  for(unsigned i = 0; i < str.size();){
    if(str[i] == ' '){i++; continue;}
    unsigned beg = i;
    while(i < str.size() && str[i] != ' ')i++;
    if(i > beg)
      tokens.push_back(str.substr(beg,i-beg));
  }
}

static void get_file_params(const char *filename, hash_map<std::string,std::string> &params){
  std::ifstream f(filename);
  if(f){
    std::string first_line;
    std::getline(f,first_line);
    // std::cout << "first line: '" << first_line << "'" << std::endl;
    if(first_line.size() >= 2 && first_line[0] == ';' && first_line[1] == '!'){
      std::vector<std::string> tokens;
      tokenize(first_line.substr(2,first_line.size()-2),tokens);
      for(unsigned i = 0; i < tokens.size(); i++){
	std::string &tok = tokens[i];
	size_t eqpos = tok.find('=');
	if(eqpos >= 0 && eqpos < tok.size()){
	  std::string left = tok.substr(0,eqpos);
	  std::string right = tok.substr(eqpos+1,tok.size()-eqpos-1);
	  params[left] = right;
        }
      }
    }
    f.close();
  }
}

extern "C" {

#if 0
  static void iZ3_write_seq(Z3_context ctx, int num, Z3_ast *cnsts, const char *filename, int num_theory, Z3_ast *theory){
    int num_fmlas = num+num_theory;
    std::vector<Z3_ast> fmlas(num_fmlas);
    if(num_theory)
      std::copy(theory,theory+num_theory,fmlas.begin());
    for(int i = 0; i < num_theory; i++)
       fmlas[i] = Z3_mk_implies(ctx,Z3_mk_true(ctx),fmlas[i]);
    std::copy(cnsts,cnsts+num,fmlas.begin()+num_theory);
    Z3_string smt = Z3_benchmark_to_smtlib_string(ctx,"none","AUFLIA","unknown","",num_fmlas-1,&fmlas[0],fmlas[num_fmlas-1]);  
    std::ofstream f(filename);
    if(num_theory)
      f << ";! THEORY=" << num_theory << "\n";
    f << smt;
    f.close();
  }

  void Z3_write_interpolation_problem(Z3_context ctx, int num, Z3_ast *cnsts, int *parents, const char *filename, int num_theory, Z3_ast *theory){
    if(!parents){
      iZ3_write_seq(ctx,num,cnsts,filename,num_theory,theory);
      return;
    }
    std::vector<Z3_ast> tcnsts(num);
    hash_map<int,Z3_ast> syms;
    for(int j = 0; j < num - 1; j++){
      std::ostringstream oss;
      oss << "$P" << j;
      std::string name = oss.str();
      Z3_symbol s = Z3_mk_string_symbol(ctx, name.c_str());
      Z3_ast symbol = Z3_mk_const(ctx, s, Z3_mk_bool_sort(ctx));
      syms[j] = symbol;
      tcnsts[j] = Z3_mk_implies(ctx,cnsts[j],symbol);
    }
    tcnsts[num-1] = Z3_mk_implies(ctx,cnsts[num-1],Z3_mk_false(ctx));
    for(int j = num-2; j >= 0; j--){
      int parent = parents[j];
      // assert(parent >= 0 && parent < num);
      tcnsts[parent] = Z3_mk_implies(ctx,syms[j],tcnsts[parent]);
    }
    iZ3_write_seq(ctx,num,&tcnsts[0],filename,num_theory,theory);
  }
#else


  static Z3_ast and_vec(Z3_context ctx,svector<Z3_ast> &c){
      return (c.size() > 1) ? Z3_mk_and(ctx,c.size(),&c[0]) : c[0];
  }
  
  static Z3_ast parents_vector_to_tree(Z3_context ctx, int num, Z3_ast *cnsts, int *parents){
    Z3_ast res;
    if(!parents){
      res = Z3_mk_interp(ctx,cnsts[0]);
      for(int i = 1; i < num-1; i++){
	Z3_ast bar[2] = {res,cnsts[i]};
	res = Z3_mk_interp(ctx,Z3_mk_and(ctx,2,bar));
      }
      if(num > 1){
	Z3_ast bar[2] = {res,cnsts[num-1]};
	res = Z3_mk_and(ctx,2,bar);
      }
    }
    else {
      std::vector<svector<Z3_ast> > chs(num);
      for(int i = 0; i < num-1; i++){
          svector<Z3_ast> &c = chs[i];
	c.push_back(cnsts[i]);
	Z3_ast foo = Z3_mk_interp(ctx,and_vec(ctx,c));
	chs[parents[i]].push_back(foo);
      }
      {
	svector<Z3_ast> &c = chs[num-1];
	c.push_back(cnsts[num-1]);
	res = and_vec(ctx,c);
      }
    }
    Z3_inc_ref(ctx,res);
    return res;
  }

  void Z3_write_interpolation_problem(Z3_context ctx, int num, Z3_ast *cnsts, int *parents, const char *filename, int num_theory, Z3_ast *theory){
    std::ofstream f(filename);
    if(num >  0){
      ptr_vector<expr> cnsts_vec(num);  // get constraints in a vector
      for(int i = 0; i < num; i++){
	expr *a = to_expr(cnsts[i]);
	cnsts_vec[i] = a;
      }
      Z3_ast tree = parents_vector_to_tree(ctx,num,cnsts,parents);
      for(int i = 0; i < num_theory; i++){
	expr *a = to_expr(theory[i]);
	cnsts_vec.push_back(a);
      }
      iz3pp(mk_c(ctx)->m(),cnsts_vec,to_expr(tree),f);
      Z3_dec_ref(ctx,tree);
    }
    f.close();

#if 0    

    
    if(!parents){
      iZ3_write_seq(ctx,num,cnsts,filename,num_theory,theory);
      return;
    }
    std::vector<Z3_ast> tcnsts(num);
    hash_map<int,Z3_ast> syms;
    for(int j = 0; j < num - 1; j++){
      std::ostringstream oss;
      oss << "$P" << j;
      std::string name = oss.str();
      Z3_symbol s = Z3_mk_string_symbol(ctx, name.c_str());
      Z3_ast symbol = Z3_mk_const(ctx, s, Z3_mk_bool_sort(ctx));
      syms[j] = symbol;
      tcnsts[j] = Z3_mk_implies(ctx,cnsts[j],symbol);
    }
    tcnsts[num-1] = Z3_mk_implies(ctx,cnsts[num-1],Z3_mk_false(ctx));
    for(int j = num-2; j >= 0; j--){
      int parent = parents[j];
      // assert(parent >= 0 && parent < num);
      tcnsts[parent] = Z3_mk_implies(ctx,syms[j],tcnsts[parent]);
    }
    iZ3_write_seq(ctx,num,&tcnsts[0],filename,num_theory,theory);
#endif

  }


#endif

  static std::vector<Z3_ast> read_cnsts;
  static std::vector<int> read_parents;
  static std::ostringstream read_error;
  static std::string read_msg;
  static std::vector<Z3_ast> read_theory;

  static bool iZ3_parse(Z3_context ctx, const char *filename, const char **error, svector<Z3_ast> &assertions){
    read_error.clear();
    try {
      std::string foo(filename);
      if(foo.size() >= 5 && foo.substr(foo.size()-5) == ".smt2"){
	Z3_ast ass = Z3_parse_smtlib2_file(ctx, filename, 0, 0, 0, 0, 0, 0);
	Z3_app app = Z3_to_app(ctx,ass);
	int nconjs = Z3_get_app_num_args(ctx,app);
	assertions.resize(nconjs);
	for(int k = 0; k < nconjs; k++)
	  assertions[k] = Z3_get_app_arg(ctx,app,k);
      }
      else {
	Z3_parse_smtlib_file(ctx, filename, 0, 0, 0, 0, 0, 0);
	int numa = Z3_get_smtlib_num_assumptions(ctx);
	int numf = Z3_get_smtlib_num_formulas(ctx);
	int num = numa + numf;
	
	assertions.resize(num);
	for(int j = 0; j < num; j++){
	  if(j < numa)
	    assertions[j] = Z3_get_smtlib_assumption(ctx,j);
	  else
	    assertions[j] = Z3_get_smtlib_formula(ctx,j-numa);
	}
      }
    }
    catch(...) {
      read_error << "SMTLIB parse error: " << Z3_get_smtlib_error(ctx);
      read_msg = read_error.str();
      *error = read_msg.c_str();
      return false;
    }
    Z3_set_error_handler(ctx, 0);
    return true;
  }
  

  int Z3_read_interpolation_problem(Z3_context ctx, int *_num, Z3_ast **cnsts, int **parents, const char *filename, const char **error, int *ret_num_theory, Z3_ast **theory ){

    hash_map<std::string,std::string> file_params;
    get_file_params(filename,file_params);
    
    unsigned num_theory = 0;
    if(file_params.find("THEORY") != file_params.end())
      num_theory = atoi(file_params["THEORY"].c_str());

    svector<Z3_ast> assertions;
    if(!iZ3_parse(ctx,filename,error,assertions))
      return false;
    
    if(num_theory > assertions.size())
        num_theory = assertions.size();
    unsigned num = assertions.size() - num_theory;

    read_cnsts.resize(num);
    read_parents.resize(num);
    read_theory.resize(num_theory);

    for(unsigned j = 0; j < num_theory; j++)
      read_theory[j] = assertions[j];
    for(unsigned j = 0; j < num; j++)
      read_cnsts[j] = assertions[j+num_theory];
    
    if(ret_num_theory)
      *ret_num_theory = num_theory;
    if(theory)
      *theory = &read_theory[0];

    if(!parents){
      *_num = num;
      *cnsts = &read_cnsts[0];
      return true;
    }

    for(unsigned j = 0; j < num; j++)
      read_parents[j] = SHRT_MAX;
    
    hash_map<Z3_ast,int> pred_map;

    for(unsigned j = 0; j < num; j++){
      Z3_ast lhs = 0, rhs = read_cnsts[j];

      if(Z3_get_decl_kind(ctx,Z3_get_app_decl(ctx,Z3_to_app(ctx,rhs))) == Z3_OP_IMPLIES){
        Z3_app app1 = Z3_to_app(ctx,rhs);
	Z3_ast lhs1 = Z3_get_app_arg(ctx,app1,0);
	Z3_ast rhs1 = Z3_get_app_arg(ctx,app1,1);
	if(Z3_get_decl_kind(ctx,Z3_get_app_decl(ctx,Z3_to_app(ctx,lhs1))) == Z3_OP_AND){
	  Z3_app app2 = Z3_to_app(ctx,lhs1);
	  int nconjs = Z3_get_app_num_args(ctx,app2);
	  for(int k = nconjs - 1; k >= 0; --k)
	    rhs1 = Z3_mk_implies(ctx,Z3_get_app_arg(ctx,app2,k),rhs1);
	  rhs = rhs1;
	}
      }

      while(1){
        Z3_app app = Z3_to_app(ctx,rhs);
        Z3_func_decl func = Z3_get_app_decl(ctx,app);
        Z3_decl_kind dk = Z3_get_decl_kind(ctx,func);
        if(dk == Z3_OP_IMPLIES){
	  if(lhs){
	    Z3_ast child = lhs;
	    if(pred_map.find(child) == pred_map.end()){
	      read_error << "formula " << j+1 << ": unknown: " << Z3_ast_to_string(ctx,child);
	      goto fail;
	    }
	    int child_num = pred_map[child];
	    if(read_parents[child_num] != SHRT_MAX){
	      read_error << "formula " << j+1 << ": multiple reference: " << Z3_ast_to_string(ctx,child);
	      goto fail;
	    }
	    read_parents[child_num] = j;
	  }
	  lhs = Z3_get_app_arg(ctx,app,0);
          rhs = Z3_get_app_arg(ctx,app,1);
	}
	else {
	  if(!lhs){
	    read_error << "formula " << j+1 << ": should be (implies {children} fmla parent)";
	    goto fail;
	  }
	  read_cnsts[j] = lhs;
	  Z3_ast name = rhs;
	  if(pred_map.find(name) != pred_map.end()){
	    read_error << "formula " << j+1 << ": duplicate symbol";
	    goto fail;
	  }
	  pred_map[name] = j;
          break;
	}
      }
    }

    for(unsigned j = 0; j < num-1; j++)
      if(read_parents[j] == SHRT_MIN){
	read_error << "formula " << j+1 << ": unreferenced";
	goto fail;
      }
    
    *_num = num;
    *cnsts = &read_cnsts[0];
    *parents = &read_parents[0];
    return true;
    
  fail:
    read_msg = read_error.str();
    *error = read_msg.c_str();
    return false;
    
  }
}
