﻿/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Polysat core saturation

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6


TODO: preserve falsification
- each rule selects a certain premises that are problematic.
  If the problematic premise is false under the current assignment, the newly inferred
  literal should also be false in the assignment in order to preserve conflicts.


TODO: when we check that 'x' is "unary":
- in principle, 'x' could be any polynomial. However, we need to divide the lhs by x, and we don't have general polynomial division yet.
  so for now we just allow the form 'value*variable'.
   (extension to arbitrary monomials for 'x' should be fairly easy too)

--*/
#include "math/polysat/saturation.h"
#include "math/polysat/solver.h"
#include "math/polysat/log.h"

namespace polysat {
    
    bool inf_saturate::perform(pvar v, conflict_core& core) {
        for (auto c1 : core) {
            if (!c1->is_ule())
                continue;
            auto c = c1.as_inequality();
            if (try_ugt_x(v, core, c))
                return true;
            if (try_ugt_y(v, core, c))
                return true;
            if (try_ugt_z(v, core, c))
                return true;
            if (try_y_l_ax_and_x_l_z(v, core, c))
                return true;
        }
        return false;
    }

    signed_constraint inf_saturate::ineq(bool is_strict, pdd const& lhs, pdd const& rhs) {
        if (is_strict)
            return s().ult(lhs, rhs);
        else
            return s().ule(lhs, rhs);
    }

    /**
    * Propagate c. It is added to reason and core all other literals in reason are false in current stack.
    * The lemmas outlined in the rules are valid and therefore c is implied.
    */
    bool inf_saturate::propagate(conflict_core& core, inequality const& crit1, inequality const& crit2, signed_constraint& c, vector<signed_constraint>& new_constraints) {
        bool crit1_false = crit1.as_signed_constraint().is_currently_false(s());
        bool crit2_false = crit2.as_signed_constraint().is_currently_false(s());
        if (!crit1_false && !crit2_false)
            return false;
        bool is_bool_false = c.bvalue(s()) == l_false;
        bool is_model_false = c.is_currently_false(s());
        if (!is_bool_false && !is_model_false)
            return false;
        
        // refresh dependencies for crit1, crit2
        // this is called before core.set, which 
        // rehashes the variable dependencies
        core.keep(crit1.as_signed_constraint());
        core.keep(crit2.as_signed_constraint());
        if (is_bool_false)
            core.insert(~c);
        else 
            core.set(c);

        // add fresh constraints
        for (auto d : new_constraints)
            core.insert(d);
        return true;
    }

    bool inf_saturate::propagate(conflict_core& core, inequality const& crit1, inequality const& crit2, bool is_strict, pdd const& lhs, pdd const& rhs, vector<signed_constraint>& new_constraints) {
        signed_constraint c = ineq(is_strict, lhs, rhs);
        return propagate(core, crit1, crit2, c, new_constraints);
    }

    /// Add premises for Ω*(x, y)
    void inf_saturate::push_omega_bisect(vector<signed_constraint>& new_constraints, pdd const& x, rational x_max, pdd const& y, rational y_max) {
        rational x_val, y_val;
        auto& pddm = x.manager();
        unsigned bit_size = pddm.power_of_2();
        rational bound = rational::power_of_two(bit_size);
        VERIFY(s().try_eval(x, x_val));
        VERIFY(s().try_eval(y, y_val));
        SASSERT(x_val * y_val < bound);

        rational x_lo = x_val, x_hi = x_max, y_lo = y_val, y_hi = y_max;
        rational two(2);
        while (x_lo < x_hi || y_lo < y_hi) {
            rational x_mid = div(x_hi + x_lo, two);
            rational y_mid = div(y_hi + y_lo, two);
            if (x_mid * y_mid >= bound)
                x_hi = x_mid - 1, y_hi = y_mid - 1;
            else
                x_lo = x_mid, y_lo = y_mid;
        }
        SASSERT(x_hi == x_lo && y_hi == y_lo);
        SASSERT(x_lo * y_lo < bound);
        SASSERT((x_lo + 1) * (y_lo + 1) >= bound);
        if ((x_lo + 1) * y_lo < bound) {
            x_hi = x_max;
            while (x_lo < x_hi) {
                rational x_mid = div(x_hi + x_lo, two);
                if (x_mid * y_lo >= bound)
                    x_hi = x_mid - 1;
                else
                    x_lo = x_mid;
            }
        }
        else if (x_lo * (y_lo + 1) < bound) {
            y_hi = y_max;
            while (y_lo < y_hi) {
                rational y_mid = div(y_hi + y_lo, two);
                if (y_mid * x_lo >= bound)
                    y_hi = y_mid - 1;
                else
                    y_lo = y_mid;
            }
        }
        SASSERT(x_lo * y_lo < bound);
        SASSERT((x_lo + 1) * y_lo >= bound);
        SASSERT(x_lo * (y_lo + 1) >= bound);

        // inequalities are justified by current assignments to x, y
        // conflict resolution should be able to pick up this as a valid justification.
        // or we resort to the same extension as in the original mul_overflow code
        // where we add explicit equality propagations from the current assignment.
        auto c1 = s().ule(x, pddm.mk_val(x_lo));
        auto c2 = s().ule(y, pddm.mk_val(y_lo));
        new_constraints.insert(c1);
        new_constraints.insert(c2);
    }

    // determine worst case upper bounds for x, y
    // then extract premises for a non-worst-case bound.
    void inf_saturate::push_omega(vector<signed_constraint>& new_constraints, pdd const& x, pdd const& y) {     
        auto& pddm = x.manager();
        unsigned bit_size = pddm.power_of_2();
        rational bound = rational::power_of_two(bit_size);
        rational x_max = bound - 1;
        rational y_max = bound - 1;

        if (x.is_var()) 
            x_max = s().m_viable.max_viable(x.var());
        if (y.is_var()) 
            y_max = s().m_viable.max_viable(y.var());        

        if (x_max * y_max  >= bound)            
            push_omega_bisect(new_constraints, x, x_max, y, y_max);
        else {
            for (auto c : s().m_cjust[y.var()])
                new_constraints.insert(c);
            for (auto c : s().m_cjust[x.var()])
                new_constraints.insert(c);
        }
    }

    /*
    * Match [v] .. <= v 
    */
    bool inf_saturate::is_l_v(pvar v, inequality const& i) {
        return i.rhs == s().var(v);
    }

    /*
    * Match [v] v <= ...
    */
    bool inf_saturate::is_g_v(pvar v, inequality const& i) {
        return i.lhs == s().var(v);
    }

    /*
    * Match [x] x <= y
    */
    bool inf_saturate::is_x_l_Y(pvar x, inequality const& c, pdd& y) {
        y = c.rhs;
        return is_g_v(x, c);
    }

    /*
    * Match [x] y <= a*x
    */
    bool inf_saturate::is_Y_l_Ax(pvar x, inequality const& d, pdd& a, pdd& y) {
        y = d.lhs;
        return is_xY(x, d.rhs, a);
    }

    bool inf_saturate::verify_Y_l_Ax(pvar x, inequality const& d, pdd const& a, pdd const& y) {        
        return d.lhs == y && d.rhs == a * s().var(x);
    }

    /**
    * Match [coeff*x] coeff*x*Y
    */

    bool inf_saturate::is_coeffxY(pdd const& x, pdd const& p, pdd& y) {
        pdd xy = x;
        return x.is_unary() && p.try_div(x.hi().val(), xy) && xy.factor(x.var(), 1, y);
    }

    /**
    * determine whether values of x * y is non-overflowing.
    */
    bool inf_saturate::is_non_overflow(pdd const& x, pdd const& y) {
        rational x_val, y_val;
        auto& pddm = x.manager();
        rational bound = rational::power_of_two(pddm.power_of_2());
        return s().try_eval(x, x_val) && s().try_eval(y, y_val) && x_val * y_val < bound;       
    }

    /**
    * Match [v] v*x <= z*x with x a variable
    */
    bool inf_saturate::is_Xy_l_XZ(pvar v, inequality const& c, pdd& x, pdd& z) {
        return is_xY(v, c.lhs, x) && is_coeffxY(x, c.rhs, z);
    }

    bool inf_saturate::verify_Xy_l_XZ(pvar v, inequality const& c, pdd const& x, pdd const& z) {
        return c.lhs == s().var(v) * x && c.rhs == z * x;
    }

    /**
    * Match [z] yx <= zx with x a variable
    */
    bool inf_saturate::is_YX_l_zX(pvar z, inequality const& c, pdd& x, pdd& y) {
        return is_xY(z, c.rhs, x) && is_coeffxY(x, c.lhs, y);
    }

    bool inf_saturate::verify_YX_l_zX(pvar z, inequality const& c, pdd const& x, pdd const& y) {
        return c.lhs == y * x && c.rhs == s().var(z) * x;
    }

    /**
    * Match [x] xY <= xZ
    */
    bool inf_saturate::is_xY_l_xZ(pvar x, inequality const& c, pdd& y, pdd& z) {
        return is_xY(x, c.lhs, y) && is_xY(x, c.rhs, z);
    }

    /**
    * Match xy = x * Y
    */
    bool inf_saturate::is_xY(pvar x, pdd const& xy, pdd& y) {
        return xy.degree(x) == 1 && xy.factor(x, 1, y);
    }

    /**
     * Implement the inferences
     *  [x] zx > yx  ==>  Ω*(x,y) \/ z > y
     *  [x] yx <= zx  ==>  Ω*(x,y) \/ y <= z \/ x = 0
     */
    bool inf_saturate::try_ugt_x(pvar v, conflict_core& core, inequality const& c) {
        pdd x = s().var(v);
        pdd y = x;
        pdd z = x;
        if (!is_xY_l_xZ(v, c, y, z))
            return false;
        if (!is_non_overflow(x, y))
            return false;
        if (!c.is_strict && s().get_value(v).is_zero())
            return false;

        vector<signed_constraint> new_constraints;   
        if (!c.is_strict)
            new_constraints.push_back(~s().eq(x));
        push_omega(new_constraints, x, y);
        return propagate(core, c, c, c.is_strict, y, z, new_constraints);
    }

    /// [y] z' <= y /\ zx > yx  ==>  Ω*(x,y) \/ zx > z'x
    /// [y] z' <= y /\ yx <= zx  ==>  Ω*(x,y) \/ z'x <= zx
    bool inf_saturate::try_ugt_y(pvar v, conflict_core& core, inequality const& le_y, inequality const& yx_l_zx, pdd const& x, pdd const& z) {
        pdd const y = s().var(v);

        SASSERT(is_l_v(v, le_y));
        SASSERT(verify_Xy_l_XZ(v, yx_l_zx, x, z));
        if (!is_non_overflow(x, y))
            return false;
          
        pdd const& z_prime = le_y.lhs;

        vector<signed_constraint> new_constraints;
        new_constraints.push_back(le_y.as_signed_constraint());
        new_constraints.push_back(yx_l_zx.as_signed_constraint());
        push_omega(new_constraints, x, y);
        // z'x <= zx
        return propagate(core, le_y, yx_l_zx, yx_l_zx.is_strict || le_y.is_strict, z_prime * x, z * x, new_constraints);
    }

    bool inf_saturate::try_ugt_y(pvar v, conflict_core& core, inequality const& c) {
        if (!is_l_v(v, c))
            return false;
        pdd x = s().var(v);
        pdd z = x;
        for (auto dd : core) {
            if (!dd->is_ule())
                continue;
            auto d = dd.as_inequality();
            if (is_Xy_l_XZ(v, d, x, z) && try_ugt_y(v, core, c, d, x, z))
                return true;
        }
        return false;
    }


    /// [x]  y <= ax /\ x <= z  (non-overflow case)
    ///     ==>   Ω*(a, z)  \/  y <= az
    bool inf_saturate::try_y_l_ax_and_x_l_z(pvar x, conflict_core& core, inequality const& c) {
        if (!is_g_v(x, c))
            return false;
        pdd y = s().var(x);
        pdd a = y;
        for (auto dd : core) {
            if (!dd->is_ule())
                continue;
            auto d = dd.as_inequality();
            if (is_Y_l_Ax(x, d, a, y) && try_y_l_ax_and_x_l_z(x, core, c, d, a, y))
                return true;
        }
        return false;
    }

    bool inf_saturate::try_y_l_ax_and_x_l_z(pvar x, conflict_core& core, inequality const& x_l_z, inequality const& y_l_ax, pdd const& a, pdd const& y) {

        SASSERT(is_g_v(x, x_l_z));
        SASSERT(verify_Y_l_Ax(x, y_l_ax, a, y));
        pdd z = x_l_z.rhs;
        if (!is_non_overflow(a, z))
            return false;
        vector<signed_constraint> new_constraints;
        new_constraints.push_back(x_l_z.as_signed_constraint());
        new_constraints.push_back(y_l_ax.as_signed_constraint());
        push_omega(new_constraints, a, z);
        return propagate(core, x_l_z, y_l_ax, x_l_z.is_strict || y_l_ax.is_strict, y, a * z, new_constraints);
    }


    /// [z] z <= y' /\ zx > yx  ==>  Ω*(x,y') \/ y'x > yx
    /// [z] z <= y' /\ yx <= zx  ==>  Ω*(x,y') \/ yx <= y'x
    bool inf_saturate::try_ugt_z(pvar z, conflict_core& core, inequality const& c) {
        if (!is_g_v(z, c))
            return false;
        pdd y = s().var(z);
        pdd x = y;
        for (auto dd : core) {
            if (!dd->is_ule())
                continue;
            auto d = dd.as_inequality();
            if (is_YX_l_zX(z, d, x, y) && try_ugt_z(z, core, c, d, x, y))
                return true;
        }
        return false;
    }

    bool inf_saturate::try_ugt_z(pvar z, conflict_core& core, inequality const& z_l_y, inequality const& yx_l_zx, pdd const& x, pdd const& y) {
        SASSERT(is_g_v(z, z_l_y));
        SASSERT(verify_YX_l_zX(z, yx_l_zx, x, y));
        pdd const& y_prime = z_l_y.rhs;
        if (!is_non_overflow(x, y_prime))
            return false;
        vector<signed_constraint> new_constraints;
        new_constraints.push_back(z_l_y.as_signed_constraint());
        new_constraints.push_back(yx_l_zx.as_signed_constraint());
        push_omega(new_constraints, x, y_prime);
        // yx <= y'x
        return propagate(core, z_l_y, yx_l_zx, z_l_y.is_strict || yx_l_zx.is_strict, y * x, y_prime * x, new_constraints);
    }

}