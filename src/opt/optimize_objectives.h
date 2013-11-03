/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    optimize_objectives.h

Abstract:
   
    Objective optimization method.

Author:

    Anh-Dung Phan (t-anphan) 2013-10-16

Notes:

--*/
#ifndef _OPT_OBJECTIVES_H_
#define _OPT_OBJECTIVES_H_

#include "opt_solver.h"

namespace opt {
    /**
       Takes solver with hard constraints added.
       Returns an optimal assignment to objective functions.
    */

    class optimize_objectives {
        ast_manager& m;
        opt_solver*  s;
        volatile bool m_cancel;
        vector<inf_eps> m_lower;
        vector<inf_eps> m_upper;
        svector<smt::theory_var> m_vars;
        symbol m_engine;
    public:
        optimize_objectives(ast_manager& m): m(m), s(0), m_cancel(false) {}

        lbool operator()(opt_solver& s, app_ref_vector& objectives, vector<inf_eps>& values);

        void set_cancel(bool f);

        void set_engine(symbol const& e) { m_engine = e; }

    private:
        
        lbool basic_opt();

        lbool farkas_opt();

        void set_max(vector<inf_eps>& dst, vector<inf_eps> const& src);

        void update_lower();

        lbool update_upper();

    };

};

#endif