/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    optsmt.h

Abstract:
   
    Objective optimization method.

Author:

    Anh-Dung Phan (t-anphan) 2013-10-16

Notes:

--*/
#ifndef _OPTSMT_H_
#define _OPTSMT_H_

#include "opt_solver.h"

namespace opt {
    /**
       Takes solver with hard constraints added.
       Returns an optimal assignment to objective functions.
    */

    class optsmt {
        ast_manager&     m;
        opt_solver*      m_s;
        volatile bool    m_cancel;
        vector<inf_eps>  m_lower;
        vector<inf_eps>  m_upper;
        app_ref_vector   m_objs;
        svector<smt::theory_var> m_vars;
        symbol           m_engine;
        model_ref        m_model;
    public:
        optsmt(ast_manager& m): m(m), m_s(0), m_cancel(false), m_objs(m) {}

        void setup(opt_solver& solver);

        lbool box();

        lbool lex(unsigned obj_index);

        lbool pareto(unsigned obj_index);

        unsigned add(app* t);

        void set_cancel(bool f);

        void updt_params(params_ref& p);

        unsigned get_num_objectives() const { return m_objs.size(); }
        void commit_assignment(unsigned index);
        inf_eps get_lower(unsigned index) const;
        inf_eps get_upper(unsigned index) const;
        void    get_model(model_ref& mdl);

        void update_lower(unsigned idx, inf_eps const& r, bool override);
        void update_upper(unsigned idx, inf_eps const& r, bool override);

        void reset();
        
    private:
        
        lbool basic_opt();

        lbool basic_lex(unsigned idx);

        lbool farkas_opt();

        void set_max(vector<inf_eps>& dst, vector<inf_eps> const& src);

        void update_lower();

        lbool update_upper();

    };

};

#endif
