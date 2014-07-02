/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    fu_malik.h

Abstract:

    Fu&Malik built-in optimization method.
    Adapted from sample code in C.

Author:

    Anh-Dung Phan (t-anphan) 2013-10-15

Notes:

    Takes solver with hard constraints added.
    Returns a maximal satisfying subset of soft_constraints
    that are still consistent with the solver state.

--*/
#ifndef _OPT_FU_MALIK_H_
#define _OPT_FU_MALIK_H_

#include "opt_solver.h"
#include "maxsmt.h"

namespace opt {

    class fu_malik : public maxsmt_solver {
        struct imp;
        imp* m_imp;
    public:
        fu_malik(ast_manager& m, opt_solver& s, expr_ref_vector& soft_constraints);
        virtual ~fu_malik();
        virtual lbool operator()();
        virtual rational get_lower() const;
        virtual rational get_upper() const;
        virtual bool get_assignment(unsigned idx) const;
        virtual void set_cancel(bool f);
        virtual void collect_statistics(statistics& st) const;
        virtual void get_model(model_ref& m);
        virtual void updt_params(params_ref& p);
    };
    
};

#endif
