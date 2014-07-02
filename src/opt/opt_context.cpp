/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    opt_context.cpp

Abstract:

    Facility for running optimization problem.

Author:

    Anh-Dung Phan (t-anphan) 2013-10-16

Notes:

--*/

#include "opt_context.h"
#include "ast_pp.h"
#include "opt_solver.h"
#include "opt_params.hpp"
#include "for_each_expr.h"
#include "goal.h"
#include "tactic.h"
#include "lia2card_tactic.h"
#include "elim01_tactic.h"
#include "solve_eqs_tactic.h"
#include "simplify_tactic.h"
#include "propagate_values_tactic.h"
#include "solve_eqs_tactic.h"
#include "elim_uncnstr_tactic.h"
#include "tactical.h"
#include "model_smt2_pp.h"
#include "card2bv_tactic.h"
#include "bvsls_opt_solver.h"
#include "nnf_tactic.h"

namespace opt {

    void context::scoped_state::push() {
        m_hard_lim.push_back(m_hard.size());
        m_objectives_lim.push_back(m_objectives.size());        
        m_objectives_term_trail_lim.push_back(m_objectives_term_trail.size());
    }

    void context::scoped_state::pop() {
        m_hard.resize(m_hard_lim.back());
        unsigned k = m_objectives_term_trail_lim.back();
        while (m_objectives_term_trail.size() > k) {
            unsigned idx = m_objectives_term_trail.back();
            m_objectives[idx].m_terms.pop_back();
            m_objectives[idx].m_weights.pop_back();
            m_objectives_term_trail.pop_back();
        }
        m_objectives_term_trail_lim.pop_back();
        k = m_objectives_lim.back();
        while (m_objectives.size() > k) {
            objective& obj = m_objectives.back();
            if (obj.m_type == O_MAXSMT) {
                m_indices.erase(obj.m_id);
            }
            m_objectives.pop_back();
        }
        m_objectives_lim.pop_back();            
        m_hard_lim.pop_back();   
    }

    
    void context::scoped_state::add(expr* hard) {
        m_hard.push_back(hard);
    }

    void context::scoped_state::set(ptr_vector<expr> & hard) {
        m_hard.reset();
        m_hard.append(hard.size(), hard.c_ptr());
    }

    unsigned context::scoped_state::add(expr* f, rational const& w, symbol const& id) {
        if (w.is_neg()) {
            throw default_exception("Negative weight supplied. Weight should be positive");
        }
        if (w.is_zero()) {
            throw default_exception("Zero weight supplied. Weight should be positive");
        }
        if (!m.is_bool(f)) {
            throw default_exception("Soft constraint should be Boolean");
        }
        if (!m_indices.contains(id)) {
            m_objectives.push_back(objective(m, id));
            m_indices.insert(id, m_objectives.size() - 1);
        }
        SASSERT(m_indices.contains(id));        
        unsigned idx = m_indices[id];
        m_objectives[idx].m_terms.push_back(f);
        m_objectives[idx].m_weights.push_back(w);
        m_objectives_term_trail.push_back(idx);
        return idx;
    }

    unsigned context::scoped_state::add(app* t, bool is_max) {
        app_ref tr(t, m);
        if (!m_bv.is_bv(t) && !m_arith.is_int_real(t)) {
            throw default_exception("Objective must be bit-vector, integer or real");   
        }
        unsigned index = m_objectives.size();
        m_objectives.push_back(objective(is_max, tr, index));
        return index;
    }

    context::context(ast_manager& m):
        m(m),
        m_arith(m),
        m_bv(m),
        m_hard_constraints(m),
        m_optsmt(m),
        m_scoped_state(m),
        m_objective_refs(m)
    {
        m_params.set_bool("model", true);
        m_params.set_bool("unsat_core", true);
        m_solver = alloc(opt_solver, m, m_params, symbol());
    }

    context::~context() {
        reset_maxsmts();
    }

    void context::reset_maxsmts() {
        map_t::iterator it = m_maxsmts.begin(), end = m_maxsmts.end();
        for (; it != end; ++it) {
            dealloc(it->m_value);
        }
    }

    void context::push() {
        m_scoped_state.push();
        m_solver->push();
    }

    void context::pop(unsigned n) {
        m_solver->pop(n);
        for (unsigned i = 0; i < n; ++i) {
            m_scoped_state.pop();
        }
    }

    void context::set_hard_constraints(ptr_vector<expr>& fmls) {
        m_scoped_state.set(fmls);
    }

    void context::add_hard_constraint(expr* f) { 
        m_scoped_state.add(f);
    }

    unsigned context::add_soft_constraint(expr* f, rational const& w, symbol const& id) { 
        return m_scoped_state.add(f, w, id);
    }

    unsigned context::add_objective(app* t, bool is_max) {
        return m_scoped_state.add(t, is_max);
    }

    void context::import_scoped_state() {
        m_optsmt.reset();        
        reset_maxsmts();
        m_objectives.reset();
        m_hard_constraints.reset();
        scoped_state& s = m_scoped_state;        
        for (unsigned i = 0; i < s.m_objectives.size(); ++i) {
            objective& obj = s.m_objectives[i];
            m_objectives.push_back(obj);
            if (obj.m_type == O_MAXSMT) {
                maxsmt* ms = alloc(maxsmt, m);
                ms->updt_params(m_params);
                m_maxsmts.insert(obj.m_id, ms);
            }
        }
        m_hard_constraints.append(s.m_hard);
    }

    lbool context::optimize() {
        if (m_pareto) {
            return execute_pareto();
        }
        import_scoped_state();
        normalize();
        internalize();
        opt_solver& s = get_solver();
        solver::scoped_push _sp(s);
        for (unsigned i = 0; i < m_hard_constraints.size(); ++i) {
            TRACE("opt", tout << "Hard constraint: " << mk_ismt2_pp(m_hard_constraints[i].get(), m) << std::endl;);
            s.assert_expr(m_hard_constraints[i].get());
        }

        IF_VERBOSE(1, verbose_stream() << "(optimize:check-sat)\n";);
        lbool is_sat = s.check_sat_core(0,0);
        TRACE("opt", tout << "initial search result: " << is_sat << "\n";);
        if (is_sat != l_true) {
            m_model = 0;
            return is_sat;
        }
        IF_VERBOSE(1, verbose_stream() << "(optimize:sat)\n";);
        s.get_model(m_model);
        m_optsmt.setup(s);
        update_lower(true);
        switch (m_objectives.size()) {
        case 0:
            return is_sat;
        case 1:
            return execute(m_objectives[0], true);
        default: {
            opt_params optp(m_params);
            symbol pri = optp.priority();
            if (pri == symbol("pareto")) {
                _sp.disable_pop();
                return execute_pareto();
            }
            else if (pri == symbol("box")) {
                return execute_box();
            }
            else {
                return execute_lex();
            }
        }
        }
    }

    void context::get_model(model_ref& mdl) {
        mdl = m_model;
        if (mdl) {
            if (m_model_converter) {
                (*m_model_converter)(mdl, 0);
            }
            get_solver().mc()(mdl, 0);
        }
    }

    lbool context::execute_min_max(unsigned index, bool committed) {
        lbool result = m_optsmt.lex(index);
        if (result == l_true && committed) m_optsmt.commit_assignment(index);
        if (result == l_true) m_optsmt.get_model(m_model);
        return result;
    }

    lbool context::execute_maxsat(symbol const& id, bool committed) {
        model_ref tmp;
        maxsmt& ms = *m_maxsmts.find(id);
        lbool result = ms(m_solver.get());
        if (result == l_true && committed) ms.commit_assignment();
        if (result != l_false && (ms.get_model(tmp), tmp.get())) ms.get_model(m_model);
        return result;
    }

    lbool context::execute(objective const& obj, bool committed) {
        switch(obj.m_type) {
        case O_MAXIMIZE: return execute_min_max(obj.m_index, committed);
        case O_MINIMIZE: return execute_min_max(obj.m_index, committed);
        case O_MAXSMT: return execute_maxsat(obj.m_id, committed);
        default: UNREACHABLE(); return l_undef;
        }
    }
    
    lbool context::execute_lex() {
        lbool r = l_true;
        for (unsigned i = 0; r == l_true && i < m_objectives.size(); ++i) {
            r = execute(m_objectives[i], i + 1 < m_objectives.size());
            if (r == l_true && !get_lower_as_num(i).is_finite()) {
                return r;
            }
        }
        DEBUG_CODE(if (r == l_true) validate_lex(););
        return r;
    }    

    lbool context::execute_box() {
        lbool r = m_optsmt.box();
        for (unsigned i = 0; r == l_true && i < m_objectives.size(); ++i) {
            objective const& obj = m_objectives[i];
            if (obj.m_type == O_MAXSMT) {
                get_solver().push();
                r = execute(obj, false);
                get_solver().pop(1);
            }
        }
        return r;
    }


    expr_ref context::mk_le(unsigned i, model_ref& mdl) {
        objective const& obj = m_objectives[i];
        expr_ref val(m), result(m), term(m);
        mk_term_val(mdl, obj, term, val);
        switch (obj.m_type) {
        case O_MINIMIZE:
            result = mk_ge(term, val);
            break;
        case O_MAXSMT:
            result = mk_ge(term, val);
            break;
        case O_MAXIMIZE:
            result = mk_ge(val, term);
            break;
        }
        return result;
    }
    
    expr_ref context::mk_ge(unsigned i, model_ref& mdl) {
        objective const& obj = m_objectives[i];
        expr_ref val(m), result(m), term(m);
        mk_term_val(mdl, obj, term, val);
        switch (obj.m_type) {
        case O_MINIMIZE:
            result = mk_ge(val, term);
            break;
        case O_MAXSMT:
            result = mk_ge(val, term);
            break;
        case O_MAXIMIZE:
            result = mk_ge(term, val);
            break;
        }
        return result;
    }
    
    expr_ref context::mk_gt(unsigned i, model_ref& mdl) {
        expr_ref result = mk_le(i, mdl);
        result = m.mk_not(result);
        return result;
    }
    
    void context::mk_term_val(model_ref& mdl, objective const& obj, expr_ref& term, expr_ref& val) {
        rational r;
        switch (obj.m_type) {
        case O_MINIMIZE:
        case O_MAXIMIZE:
            term = obj.m_term;
            break;
        case O_MAXSMT: {
            unsigned sz = obj.m_terms.size();
            expr_ref_vector sum(m);
            expr_ref zero(m);
            zero = m_arith.mk_numeral(rational(0), false);
            for (unsigned i = 0; i < sz; ++i) {
                expr* t = obj.m_terms[i];
                rational const& w = obj.m_weights[i];
                sum.push_back(m.mk_ite(t, m_arith.mk_numeral(w, false), zero));                    
            }
            if (sum.empty()) {
                term = zero;
            }
            else {
                term = m_arith.mk_add(sum.size(), sum.c_ptr());
            }                           
            break;
        }
        }
        VERIFY(mdl->eval(term, val) && is_numeral(val, r));
    }        


    expr_ref context::mk_ge(expr* t, expr* s) {
        expr_ref result(m);
        if (m_bv.is_bv(t)) {
            result = m_bv.mk_ule(s, t);
        }
        else {
            result = m_arith.mk_ge(t, s);
        }
        return result;
    }

    void context::yield() {
        m_pareto->get_model(m_model);
        update_bound(true, true);
        update_bound(true, false);
    }

    lbool context::execute_pareto() {
        if (!m_pareto) {
            m_pareto = alloc(gia_pareto, m, *this, m_solver.get(), m_params);
        }
        lbool is_sat = (*(m_pareto.get()))();
        if (is_sat != l_true) {
            m_pareto = 0;
        }
        if (is_sat == l_true) {
            yield();
        }
        else {
            m_solver->pop(1);
        }
        return is_sat;
        // NB. fix race condition for set_cancel
    }

    void context::display_bounds(std::ostream& out, bounds_t const& b) const {
        for (unsigned i = 0; i < m_objectives.size(); ++i) {
            objective const& obj = m_objectives[i];
            display_objective(out, obj);
            if (obj.m_type == O_MAXIMIZE) {
                out << " |-> [" << b[i].first << ":" << b[i].second << "]\n";
            }
            else {
                out << " |-> [" << -b[i].second << ":" << -b[i].first << "]\n";
            }
        }        
    }

    opt_solver& context::get_solver() { 
        return *m_solver.get(); 
    }

    bool context::is_numeral(expr* e, rational & n) const {
        unsigned sz;
        return m_arith.is_numeral(e, n) || m_bv.is_numeral(e, n, sz);
    }

    void context::normalize() {
        expr_ref_vector fmls(m);
        to_fmls(fmls);
        simplify_fmls(fmls);
        from_fmls(fmls);
    }

    void context::simplify_fmls(expr_ref_vector& fmls) {
        goal_ref g(alloc(goal, m, true, false));
        for (unsigned i = 0; i < fmls.size(); ++i) {
            g->assert_expr(fmls[i].get());
        }
        tactic_ref tac0 = 
            and_then(mk_simplify_tactic(m), 
                     mk_propagate_values_tactic(m),
                     mk_solve_eqs_tactic(m),
                     mk_elim_uncnstr_tactic(m),
                     mk_simplify_tactic(m));   
        opt_params optp(m_params);
        tactic_ref tac2, tac3;
        if (optp.engine() == "bvsls") {
            tac2 = mk_elim01_tactic(m);
            tac3 = mk_lia2card_tactic(m);
            params_ref lia_p;
            lia_p.set_bool("compile_equality", optp.pb_compile_equality());
            tac3->updt_params(lia_p);
            m_simplify = and_then(tac0.get(), tac2.get(), tac3.get(),
                                  mk_card2bv_tactic(m),                                  
                                  mk_simplify_tactic(m),                                  
                                  mk_nnf_tactic(m));
            m_solver = alloc(bvsls_opt_solver, m, m_params);
        }
        else if (optp.elim_01()) {
            tac2 = mk_elim01_tactic(m);
            tac3 = mk_lia2card_tactic(m);
            params_ref lia_p;
            lia_p.set_bool("compile_equality", optp.pb_compile_equality());
            tac3->updt_params(lia_p);
            m_simplify = and_then(tac0.get(), tac2.get(), tac3.get());
        }
        else {
            m_simplify = tac0.get();
        }
        proof_converter_ref pc;
        expr_dependency_ref core(m);
        goal_ref_buffer result;
        (*m_simplify)(g, result, m_model_converter, pc, core); 
        SASSERT(result.size() == 1);
        goal* r = result[0];
        fmls.reset();
        expr_ref tmp(m);
        for (unsigned i = 0; i < r->size(); ++i) {
            fmls.push_back(r->form(i));
        }        
    }

    bool context::is_maximize(expr* fml, app_ref& term, expr*& orig_term, unsigned& index) {
        if (is_app(fml) && m_objective_fns.find(to_app(fml)->get_decl(), index) && 
            m_objectives[index].m_type == O_MAXIMIZE) {
            term = to_app(to_app(fml)->get_arg(0));
            orig_term = m_objective_orig.find(to_app(fml)->get_decl());
            return true;
        }
        return false;
    }

    bool context::is_minimize(expr* fml, app_ref& term, expr*& orig_term, unsigned& index) {
        if (is_app(fml) && m_objective_fns.find(to_app(fml)->get_decl(), index) && 
            m_objectives[index].m_type == O_MINIMIZE) {
            term = to_app(to_app(fml)->get_arg(0));
            orig_term = m_objective_orig.find(to_app(fml)->get_decl());
            return true;
        }
        return false;
    }

    bool context::is_maxsat(expr* fml, expr_ref_vector& terms, 
                            vector<rational>& weights, rational& offset, 
                            bool& neg, symbol& id, unsigned& index) {
        if (!is_app(fml)) return false;
        neg = false;
        app* a = to_app(fml);
        if (m_objective_fns.find(a->get_decl(), index) && m_objectives[index].m_type == O_MAXSMT) {
            for (unsigned i = 0; i < a->get_num_args(); ++i) {
                expr* arg = a->get_arg(i);
                if (m.is_true(arg)) {

                }
                else if (m.is_false(arg)) {
                    offset += m_objectives[index].m_weights[i];
                }
                else {
                    terms.push_back(arg);
                    weights.push_back(m_objectives[index].m_weights[i]);
                }
            } 
            id = m_objectives[index].m_id;
            return true;
        }
        app_ref term(m);
        expr* orig_term;
        offset = rational::zero();
        bool is_max = is_maximize(fml, term, orig_term, index);
        bool is_min = !is_max && is_minimize(fml, term, orig_term, index);
        if (is_min && get_pb_sum(term, terms, weights, offset)) {
            TRACE("opt", tout << "try to convert minimization" << mk_pp(term, m) << "\n";);
            // minimize 2*x + 3*y 
            // <=>
            // (assert-soft (not x) 2)
            // (assert-soft (not y) 3)
            //
            for (unsigned i = 0; i < weights.size(); ++i) {
                if (weights[i].is_neg()) {
                    offset += weights[i];
                    weights[i].neg();
                }
                else {
                    terms[i] = m.mk_not(terms[i].get());
                }
            }
            TRACE("opt", 
                  tout << "Convert minimization " << mk_pp(orig_term, m) << "\n";
                  tout << "to maxsat: " << term << "\n";
                  for (unsigned i = 0; i < weights.size(); ++i) {
                      tout << mk_pp(terms[i].get(), m) << ": " << weights[i] << "\n";
                  }
                  tout << "offset: " << offset << "\n";
                  );
            std::ostringstream out;
            out << mk_pp(orig_term, m);
            id = symbol(out.str().c_str());
            return true;
        }
        if (is_max && get_pb_sum(term, terms, weights, offset)) {
            TRACE("opt", tout << "try to convert maximization" << mk_pp(term, m) << "\n";);
            // maximize 2*x + 3*y - z 
            // <=>
            // (assert-soft x 2)
            // (assert-soft y 3)
            // (assert-soft (not z) 1)
            // offset := 6
            // maximize = offset - penalty
            // 
            for (unsigned i = 0; i < weights.size(); ++i) {
                if (weights[i].is_neg()) {
                    weights[i].neg();
                    terms[i] = m.mk_not(terms[i].get());
                }
                offset += weights[i];
            }
            neg = true;
            std::ostringstream out;
            out << mk_pp(orig_term, m);
            id = symbol(out.str().c_str());
            return true;
        }
        if ((is_max || is_min) && m_bv.is_bv(term)) {
            offset.reset();
            unsigned bv_size = m_bv.get_bv_size(term);
            expr_ref val(m);
            val = m_bv.mk_numeral(is_max, 1);
            for (unsigned i = 0; i < bv_size; ++i) {
                rational w = power(rational(2),i);
                weights.push_back(w);
                terms.push_back(m.mk_eq(val, m_bv.mk_extract(i, i, term)));
                if (is_max) {
                    offset += w;
                }
            }
            neg = is_max;
            std::ostringstream out;
            out << mk_pp(orig_term, m);
            id = symbol(out.str().c_str());
            return true;            
        }
        return false;
    }

    expr* context::mk_objective_fn(unsigned index, objective_t ty, unsigned sz, expr*const* args) {
        ptr_vector<sort> domain;
        for (unsigned i = 0; i < sz; ++i) {
            domain.push_back(m.get_sort(args[i]));
        }
        char const* name = "";
        switch(ty) {
        case O_MAXIMIZE: name = "maximize"; break;
        case O_MINIMIZE: name = "minimize"; break;
        case O_MAXSMT: name = "maxsat"; break;
        default: break;
        }
        func_decl* f = m.mk_fresh_func_decl(name,"", domain.size(), domain.c_ptr(), m.mk_bool_sort());
        m_objective_fns.insert(f, index);
        m_objective_refs.push_back(f);
        if (sz > 0) {
            m_objective_orig.insert(f, args[0]);
        }
        return m.mk_app(f, sz, args);
    }

    expr* context::mk_maximize(unsigned index, app* t) {
        expr* t_ = t;
        return mk_objective_fn(index, O_MAXIMIZE, 1, &t_);
    }

    expr* context::mk_minimize(unsigned index, app* t) {
        expr* t_ = t;
        return mk_objective_fn(index, O_MINIMIZE, 1, &t_);
    }

    expr* context::mk_maxsat(unsigned index, unsigned num_fmls, expr* const* fmls) {
        return mk_objective_fn(index, O_MAXSMT, num_fmls, fmls);
    }

    void context::from_fmls(expr_ref_vector const& fmls) {
        TRACE("opt",
              for (unsigned i = 0; i < fmls.size(); ++i) {
                  tout << mk_pp(fmls[i], m) << "\n";
              });
        m_hard_constraints.reset();
        expr* orig_term;
        for (unsigned i = 0; i < fmls.size(); ++i) {
            expr* fml = fmls[i];
            app_ref tr(m);
            expr_ref_vector terms(m);
            vector<rational> weights;
            rational offset(0);
            unsigned index;
            symbol id;
            bool neg;
            if (is_maxsat(fml, terms, weights, offset, neg, id, index)) {
                objective& obj = m_objectives[index];
                if (obj.m_type != O_MAXSMT) {
                    // change from maximize/minimize.
                    obj.m_id = id;
                    obj.m_type = O_MAXSMT;
                    obj.m_weights.append(weights);
                    SASSERT(!m_maxsmts.contains(id));
                    maxsmt* ms = alloc(maxsmt, m);
                    ms->updt_params(m_params);
                    m_maxsmts.insert(id, ms);
                }
                SASSERT(obj.m_id == id);
                obj.m_terms.reset();
                obj.m_terms.append(terms);
                obj.m_offset = offset;
                obj.m_neg = neg;
                TRACE("opt", tout << "maxsat: " << id << " offset:" << offset << "\n";);
            }
            else if (is_maximize(fml, tr, orig_term, index)) {
                m_objectives[index].m_term = tr;
            }
            else if (is_minimize(fml, tr, orig_term, index)) {
                m_objectives[index].m_term = tr;
            }
            else {
                m_hard_constraints.push_back(fml);
            }
        }
    }

    void context::to_fmls(expr_ref_vector& fmls) {
        m_objective_fns.reset();
        fmls.append(m_hard_constraints);
        for (unsigned i = 0; i < m_objectives.size(); ++i) {
            objective const& obj = m_objectives[i];
            switch(obj.m_type) {
            case O_MINIMIZE:
                fmls.push_back(mk_minimize(i, obj.m_term));
                break;
            case O_MAXIMIZE:
                fmls.push_back(mk_maximize(i, obj.m_term));
                break;
            case O_MAXSMT: 
                fmls.push_back(mk_maxsat(i, obj.m_terms.size(), obj.m_terms.c_ptr()));
                break;
            }
        }
        TRACE("opt",
              for (unsigned i = 0; i < fmls.size(); ++i) {
                  tout << mk_pp(fmls[i].get(), m) << "\n";
              });
    }

    void context::internalize() {
        for (unsigned i = 0; i < m_objectives.size(); ++i) {
            objective & obj = m_objectives[i];
            switch(obj.m_type) {
            case O_MINIMIZE: {
                app_ref tmp(m);
                tmp = m_arith.mk_uminus(obj.m_term);
                obj.m_index = m_optsmt.add(tmp);
                break;
            }
            case O_MAXIMIZE:
                obj.m_index = m_optsmt.add(obj.m_term);
                break;
            case O_MAXSMT: {
                maxsmt& ms = *m_maxsmts.find(obj.m_id);
                for (unsigned j = 0; j < obj.m_terms.size(); ++j) {
                    ms.add(obj.m_terms[j].get(), obj.m_weights[j]);        
                }
                break;
            }
            }
        }
    }

    void context::update_bound(bool override, bool is_lower) {
        expr_ref val(m);
        for (unsigned i = 0; i < m_objectives.size(); ++i) {
            objective const& obj = m_objectives[i];
            rational r;
            switch(obj.m_type) {
            case O_MINIMIZE:
                if (m_model->eval(obj.m_term, val) && is_numeral(val, r)) {
                    r += obj.m_offset;
                    if (is_lower) {
                        m_optsmt.update_lower(obj.m_index, inf_eps(-r), override);
                    }
                    else {
                        m_optsmt.update_upper(obj.m_index, inf_eps(-r), override);
                    }
                }
                break;
            case O_MAXIMIZE:
                if (m_model->eval(obj.m_term, val) && is_numeral(val, r)) {
                    r += obj.m_offset;
                    if (is_lower) {
                        m_optsmt.update_lower(obj.m_index, inf_eps(r), override);
                    }
                    else {
                        m_optsmt.update_upper(obj.m_index, inf_eps(r), override);
                    }
                }
                break;
            case O_MAXSMT: {
                bool ok = true;
                for (unsigned j = 0; ok && j < obj.m_terms.size(); ++j) {
                    if (m_model->eval(obj.m_terms[j], val)) {
                        if (!m.is_true(val)) {
                            r += obj.m_weights[j];
                        }
                    }
                    else {
                        ok = false;
                    }
                }
                if (ok) {
                    if (is_lower) {
                        m_maxsmts.find(obj.m_id)->update_upper(r, override);
                    }
                    else {
                        m_maxsmts.find(obj.m_id)->update_lower(r, override);
                    }
                }
                break;
            }
            }
        }
    }

    void context::display(std::ostream& out) {
        display_assignment(out);
    }

    void context::display_assignment(std::ostream& out) {
        for (unsigned i = 0; i < m_scoped_state.m_objectives.size(); ++i) {
            objective const& obj = m_scoped_state.m_objectives[i];
            display_objective(out, obj);
            if (get_lower_as_num(i) != get_upper_as_num(i)) {
                out << " |-> [" << get_lower(i) << ":" << get_upper(i) << "]\n";
            }
            else {
                out << " |-> " << get_lower(i) << "\n";
            }
        }
    }

    void context::display_objective(std::ostream& out, objective const& obj) const {
        switch(obj.m_type) {
        case O_MAXSMT: {
            symbol s = obj.m_id;
            if (s != symbol::null) {
                out << s;
            }
            break;
        }
        default:
            out << obj.m_term;
            break;
        }
    }

    inf_eps context::get_lower_as_num(unsigned idx) {
        if (idx > m_objectives.size()) {
            throw default_exception("index out of bounds"); 
        }
        objective const& obj = m_objectives[idx];
        switch(obj.m_type) {
        case O_MAXSMT: {
            rational r = m_maxsmts.find(obj.m_id)->get_lower();
            TRACE("opt", tout << "maxsmt: " << r << " negate: " << obj.m_neg << " offset: " << obj.m_offset << "\n";);
            if (obj.m_neg) r.neg();
            r += obj.m_offset;
            return inf_eps(r);
        }
        case O_MINIMIZE:
            return -m_optsmt.get_upper(obj.m_index);
        case O_MAXIMIZE: 
            return m_optsmt.get_lower(obj.m_index);
        default:
            UNREACHABLE();
            return inf_eps();
        }        
    }

    inf_eps context::get_upper_as_num(unsigned idx) {
        if (idx > m_objectives.size()) {
            throw default_exception("index out of bounds"); 
        }
        objective const& obj = m_objectives[idx];
        switch(obj.m_type) {
        case O_MAXSMT: {
            rational r = m_maxsmts.find(obj.m_id)->get_upper();
            if (obj.m_neg) r.neg();
            r += obj.m_offset;
            return inf_eps(r);
        }
        case O_MINIMIZE:
            return -m_optsmt.get_lower(obj.m_index);
        case O_MAXIMIZE: 
            return m_optsmt.get_upper(obj.m_index);
        default:
            UNREACHABLE();
            return inf_eps();
        }
    }

    expr_ref context::get_lower(unsigned idx) {
        return to_expr(get_lower_as_num(idx));
    }

    expr_ref context::get_upper(unsigned idx) {
        return to_expr(get_upper_as_num(idx));
    }

    expr_ref context::to_expr(inf_eps const& n) {
        rational inf = n.get_infinity();
        rational r   = n.get_rational();
        rational eps = n.get_infinitesimal();
        expr_ref_vector args(m);
        if (!inf.is_zero()) {
            expr* oo = m.mk_const(symbol("oo"), m_arith.mk_int());
            if (inf.is_one()) {
                args.push_back(oo);
            }
            else {
                args.push_back(m_arith.mk_mul(m_arith.mk_numeral(inf, inf.is_int()), oo));
            }
        }
        if (!r.is_zero()) {
            args.push_back(m_arith.mk_numeral(r, r.is_int()));
        }
        if (!eps.is_zero()) {
            expr* ep = m.mk_const(symbol("epsilon"), m_arith.mk_int());
            if (eps.is_one()) {
                args.push_back(ep);
            }
            else {
                args.push_back(m_arith.mk_mul(m_arith.mk_numeral(eps, eps.is_int()), ep));
            }
        }
        switch(args.size()) {
        case 0: return expr_ref(m_arith.mk_numeral(rational(0), true), m);
        case 1: return expr_ref(args[0].get(), m);
        default: return expr_ref(m_arith.mk_add(args.size(), args.c_ptr()), m);
        }
    }
        
    void context::set_cancel(bool f) {
        if (m_solver) {
            m_solver->set_cancel(f);
        }
        if (m_simplify) {
            m_simplify->set_cancel(f);
        }
        if (m_pareto) {
            m_pareto->set_cancel(f);
        }
        m_optsmt.set_cancel(f);
        map_t::iterator it = m_maxsmts.begin(), end = m_maxsmts.end();
        for (; it != end; ++it) {
            it->m_value->set_cancel(f);
        }
    }

    void context::collect_statistics(statistics& stats) const {
        if (m_solver) {
            m_solver->collect_statistics(stats);
        }
        if (m_simplify) {
            m_simplify->collect_statistics(stats);
        }
        map_t::iterator it = m_maxsmts.begin(), end = m_maxsmts.end();
        for (; it != end; ++it) {
            it->m_value->collect_statistics(stats);
        }        
    }

    void context::collect_param_descrs(param_descrs & r) {
        opt_params::collect_param_descrs(r);
    }
    
    void context::updt_params(params_ref& p) {
        m_params.append(p);
        if (m_solver) {
            m_solver->updt_params(m_params);
        }
        m_optsmt.updt_params(m_params);
        map_t::iterator it = m_maxsmts.begin(), end = m_maxsmts.end();
        for (; it != end; ++it) {
            it->m_value->updt_params(m_params);
        }
    }

    typedef obj_hashtable<func_decl> func_decl_set;

    struct context::free_func_visitor {
        ast_manager& m;
        func_decl_set m_funcs;
        obj_hashtable<sort> m_sorts;
        expr_mark m_visited;
    public:
        free_func_visitor(ast_manager& m): m(m) {}
        void operator()(var * n)        { }
        void operator()(app * n)        { 
            if (n->get_family_id() == null_family_id) {
                m_funcs.insert(n->get_decl()); 
            }
            sort* s = m.get_sort(n);
            if (s->get_family_id() == null_family_id) {
                m_sorts.insert(s);
            }
        }
        void operator()(quantifier * n) { }
        func_decl_set& funcs() { return m_funcs; }
        obj_hashtable<sort>& sorts() { return m_sorts; }

        void collect(expr* e) {
            for_each_expr(*this, m_visited, e);
        }
    };

    std::string context::to_string() const {
        smt2_pp_environment_dbg env(m);
        free_func_visitor visitor(m);
        std::ostringstream out;
#define PP(_e_) ast_smt2_pp(out, _e_, env);
        for (unsigned i = 0; i < m_scoped_state.m_hard.size(); ++i) {
            visitor.collect(m_scoped_state.m_hard[i]);
        }
        for (unsigned i = 0; i < m_scoped_state.m_objectives.size(); ++i) {
            objective const& obj = m_scoped_state.m_objectives[i];
            switch(obj.m_type) {
            case O_MAXIMIZE: 
            case O_MINIMIZE:
                visitor.collect(obj.m_term);
                break;
            case O_MAXSMT: 
                for (unsigned j = 0; j < obj.m_terms.size(); ++j) {
                    visitor.collect(obj.m_terms[j]);
                }
                break;
            default: 
                UNREACHABLE();
                break;
            }
        }

        obj_hashtable<sort>::iterator sit = visitor.sorts().begin();
        obj_hashtable<sort>::iterator send = visitor.sorts().end();
        for (; sit != send; ++sit) {
            PP(*sit);
        }
        func_decl_set::iterator it  = visitor.funcs().begin();
        func_decl_set::iterator end = visitor.funcs().end();
        for (; it != end; ++it) {
            PP(*it);
            out << "\n";
        }
        for (unsigned i = 0; i < m_scoped_state.m_hard.size(); ++i) {
            out << "(assert ";
            PP(m_scoped_state.m_hard[i]);
            out << ")\n";
        }
        for (unsigned i = 0; i < m_scoped_state.m_objectives.size(); ++i) {
            objective const& obj = m_scoped_state.m_objectives[i];
            switch(obj.m_type) {
            case O_MAXIMIZE: 
                out << "(maximize ";
                PP(obj.m_term);
                out << ")\n";
                break;
            case O_MINIMIZE:
                out << "(minimize ";
                PP(obj.m_term);
                out << ")\n";
                break;
            case O_MAXSMT: 
                for (unsigned j = 0; j < obj.m_terms.size(); ++j) {
                    out << "(assert-soft ";
                    PP(obj.m_terms[j]);
                    rational w = obj.m_weights[j];
                    if (w.is_int()) {
                        out << " :weight " << w;
                    }
                    else {
                        out << " :dweight " << w;
                    }
                    if (obj.m_id != symbol::null) {
                        out << " :id " << obj.m_id;
                    }
                    out << ")\n";
                }
                break;
            default: 
                UNREACHABLE();
                break;
            }
        }        
        out << "(check-sat)\n"; 
        return out.str();
    }

    void context::validate_lex() {
        rational r1;
        expr_ref val(m);
        for (unsigned i = 0; i < m_objectives.size(); ++i) {
            objective const& obj = m_objectives[i];
            switch(obj.m_type) {
            case O_MINIMIZE:
            case O_MAXIMIZE: {
                inf_eps n = m_optsmt.get_lower(obj.m_index);
                if (n.get_infinity().is_zero() &&
                    n.get_infinitesimal().is_zero() &&
                    m_model->eval(obj.m_term, val) &&
                    is_numeral(val, r1)) {
                    rational r2 = n.get_rational();
                    if (obj.m_type == O_MINIMIZE) {
                        r1.neg();
                    }
                    CTRACE("opt", r1 != r2, tout << obj.m_term << " evaluates to " << r1 << " but has objective " << r2 << "\n";);
                    CTRACE("opt", r1 != r2, model_smt2_pp(tout, m, *m_model, 0););
                    SASSERT(r1 == r2);
                }
                break;
            }
            case O_MAXSMT: {
                maxsmt& ms = *m_maxsmts.find(obj.m_id);
                for (unsigned i = 0; i < obj.m_terms.size(); ++i) {
                    VERIFY(m_model->eval(obj.m_terms[i], val));
                    CTRACE("opt",ms.get_assignment(i) != (m.mk_true() == val), 
                           tout << mk_pp(obj.m_terms[i], m) << " evaluates to " << val << "\n";
                           model_smt2_pp(tout, m, *m_model, 0););
                    SASSERT(ms.get_assignment(i) == (m.mk_true() == val));
                }
                break;
            }
            }       
        } 
    }
}
