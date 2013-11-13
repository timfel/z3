/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    network_flow_def.h

Abstract:

    Implements Network Simplex algorithm for min cost flow problem

Author:

    Anh-Dung Phan (t-anphan) 2013-10-24

Notes:
   
--*/

#ifndef _NETWORK_FLOW_DEF_H_
#define _NETWORK_FLOW_DEF_H_

#include"network_flow.h"
#include"uint_set.h"

namespace smt {

    template<typename Ext>
    network_flow<Ext>::network_flow(graph & g, vector<fin_numeral> const & balances) :
        m_balances(balances),
        m_tree(m_graph) {
        // Network flow graph has the edges in the reversed order compared to constraint graph
        // We only take enabled edges from the original graph
        for (unsigned i = 0; i < g.get_num_nodes(); ++i) {
            m_graph.init_var(i);
        }
        vector<edge> const & es = g.get_all_edges();
        for (unsigned i = 0; i < es.size(); ++i) {
            edge const & e = es[i];
            if (e.is_enabled()) {
                m_graph.add_edge(e.get_target(), e.get_source(), e.get_weight(), explanation());
            }
        }
        m_step = 0;
    }

    template<typename Ext>
    void network_flow<Ext>::initialize() {
        TRACE("network_flow", tout << "initialize...\n";);
        // Create an artificial root node to construct initial spanning tree
        unsigned num_nodes = m_graph.get_num_nodes();
        unsigned num_edges = m_graph.get_num_edges();
        
        node root = num_nodes;
        m_graph.init_var(root);

        m_potentials.resize(num_nodes + 1);     
        m_potentials[root] = numeral::zero();

        m_balances.resize(num_nodes + 1);
        fin_numeral sum_supply = fin_numeral::zero();
        for (unsigned i = 0; i < num_nodes; ++i) {
            sum_supply += m_balances[i];
        }        
        m_balances[root] = -sum_supply;

        m_flows.resize(num_nodes + num_edges);
        m_flows.fill(numeral::zero());
        m_states.resize(num_nodes + num_edges);
        m_states.fill(LOWER);

        // Create artificial edges from/to root node to/from other nodes and initialize the spanning tree
        svector<edge_id> tree;
        bool is_forward;
        for (unsigned i = 0; i < num_nodes; ++i) {
            is_forward = !m_balances[i].is_neg();
            m_states[num_edges + i] = BASIS;
            node src = is_forward ? i : root;
            node tgt = is_forward ? root : i;            
            m_flows[num_edges + i] = is_forward ? m_balances[i] : -m_balances[i];
            m_potentials[i] = is_forward ? numeral::one() : -numeral::one();
            tree.push_back(m_graph.add_edge(src, tgt, numeral::one(), explanation()));
        }

        m_tree.initialize(tree);

        TRACE("network_flow", {
                tout << pp_vector("Potentials", m_potentials, true) << pp_vector("Flows", m_flows);
            });
        TRACE("network_flow", tout << "Spanning tree:\n" << display_spanning_tree(););
        SASSERT(check_well_formed());
    }

    template<typename Ext>
    void network_flow<Ext>::update_potentials() {        
        node src = m_graph.get_source(m_enter_id);
        node tgt = m_graph.get_target(m_enter_id);      
        numeral cost = m_potentials[src] - m_potentials[tgt] - m_graph.get_weight(m_enter_id);
        numeral change = m_tree.in_subtree_t2(tgt) ? cost : -cost;
        node start = m_graph.get_source(m_leave_id);
        if (!m_tree.in_subtree_t2(start)) {
            start = m_graph.get_target(m_leave_id);;
        }
        TRACE("network_flow", tout << "update_potentials of T_" << start << " with change = " << change << "...\n";);
        svector<node> descendants;
        m_tree.get_descendants(start, descendants);
        SASSERT(descendants.size() >= 1);
        for (unsigned i = 0; i < descendants.size(); ++i) {
            node u = descendants[i];           
            m_potentials[u] += change;
        }
        TRACE("network_flow", tout << pp_vector("Potentials", m_potentials, true););
    }

    template<typename Ext>
    void network_flow<Ext>::update_flows() {
        TRACE("network_flow", tout << "update_flows...\n";);
        m_flows[m_enter_id] += *m_delta;
        node src = m_graph.get_source(m_enter_id);
        node tgt = m_graph.get_target(m_enter_id); 
        svector<edge_id> path;
        svector<bool> against;
        m_tree.get_path(src, tgt, path, against);
        SASSERT(path.size() >= 1);
        for (unsigned i = 0; i < path.size(); ++i) {
            edge_id e_id = path[i];
            m_flows[e_id] += against[i] ? - *m_delta : *m_delta;
        }
        TRACE("network_flow", tout << pp_vector("Flows", m_flows, true););
    }
    
    template<typename Ext>
    bool network_flow<Ext>::choose_leaving_edge() {
        TRACE("network_flow", tout << "choose_leaving_edge...\n";);
        node src = m_graph.get_source(m_enter_id);
        node tgt = m_graph.get_target(m_enter_id); 
        m_delta.set_invalid();
        edge_id leave_id;
        svector<edge_id> path;
        svector<bool> against;
        m_tree.get_path(src, tgt, path, against);
        SASSERT(path.size() >= 1);
        for (unsigned i = 0; i < path.size(); ++i) {
            edge_id e_id = path[i];
            if (against[i] && (!m_delta || m_flows[e_id] < *m_delta)) {
                m_delta = m_flows[e_id];
                leave_id = e_id;
            }
        }

        if (m_delta) {
            m_leave_id = leave_id;

            TRACE("network_flow", { 
                tout << "Found leaving edge " << m_leave_id;
                tout << " between node " << m_graph.get_source(m_leave_id);
                tout << " and node " << m_graph.get_target(m_leave_id) << " with delta = " << *m_delta << "...\n";
                });
            return true;
        }
        TRACE("network_flow", tout << "Can't find a leaving edge... The problem is unbounded.\n";);
        return false;
    }

    template<typename Ext>
    void network_flow<Ext>::update_spanning_tree() {  
        m_tree.update(m_enter_id, m_leave_id);
    }

    // FIXME: should declare pivot as a pivot_rule_impl and refactor
    template<typename Ext>
    bool network_flow<Ext>::choose_entering_edge(pivot_rule pr) {
        if (pr == FIRST_ELIGIBLE) {
            first_eligible_pivot pivot(m_graph, m_potentials, m_states, m_enter_id);
            return pivot.choose_entering_edge();
        }
        else if (pr == BEST_ELIGIBLE) {
            best_eligible_pivot pivot(m_graph, m_potentials, m_states, m_enter_id);
            return pivot.choose_entering_edge();
        }
        else {
            candidate_list_pivot pivot(m_graph, m_potentials, m_states, m_enter_id);
            return pivot.choose_entering_edge();
        } 
    }

    // Minimize cost flows
    // Return true if found an optimal solution, and return false if unbounded
    template<typename Ext>
    bool network_flow<Ext>::min_cost(pivot_rule pr) {
        initialize();
        while (choose_entering_edge(pr)) { 
            bool bounded = choose_leaving_edge();
            if (!bounded) return false;
            update_flows();
            if (m_enter_id != m_leave_id) {
				SASSERT(edge_in_tree(m_leave_id));
				SASSERT(!edge_in_tree(m_enter_id));
                m_states[m_enter_id] = BASIS;
                m_states[m_leave_id] = (m_flows[m_leave_id].is_zero()) ? LOWER : UPPER;
                update_spanning_tree();
                update_potentials();                
                TRACE("network_flow", tout << "Spanning tree:\n" << display_spanning_tree(););
                SASSERT(check_well_formed());
            } 
            else {
                m_states[m_leave_id] = m_states[m_leave_id] == LOWER ? UPPER : LOWER;
            }            
        }
        TRACE("network_flow", tout << "Found optimal solution.\n";);
        SASSERT(check_optimal());
        return true;
    }

    // Get the optimal solution
    template<typename Ext>
    typename network_flow<Ext>::numeral network_flow<Ext>::get_optimal_solution(vector<numeral> & result, bool is_dual) {
        numeral objective_value = numeral::zero();
        unsigned num_edges = m_graph.get_num_edges();
        for (unsigned i = 0; i < num_edges; ++i) {
            if (m_states[i] == BASIS) 
            {
                objective_value += m_graph.get_weight(i).get_rational() * m_flows[i];
            }
        }
        result.reset();
        if (is_dual) {            
            result.append(m_potentials);                   
        }
        else {
            result.append(m_flows);     
        }
        return objective_value;
    }

    template<typename Ext>
    bool network_flow<Ext>::edge_in_tree(edge_id id) const {
        return m_states[id] == BASIS;
    }
    
    template<typename Ext>
    bool network_flow<Ext>::check_well_formed() {
        SASSERT(m_tree.check_well_formed());
        SASSERT(!m_delta || !(*m_delta).is_neg());

        // m_flows are zero on non-basic edges
        for (unsigned i = 0; i < m_flows.size(); ++i) {
            SASSERT(!m_flows[i].is_neg());
            SASSERT(m_states[i] == BASIS || m_flows[i].is_zero());
        }

        unsigned num_edges = m_graph.get_num_edges();
        for (unsigned i = 0; i < num_edges; ++i) {
            if (m_states[i] == BASIS) {
                SASSERT(m_potentials[m_graph.get_source(i)] - m_potentials[m_graph.get_target(i)] == m_graph.get_weight(i));
            }
        }

        return true;
    }

    template<typename Ext>
    bool network_flow<Ext>::check_optimal() {
        numeral total_cost = numeral::zero();
        unsigned num_edges = m_graph.get_num_edges();
        for (unsigned i = 0; i < num_edges; ++i) {
            if (m_states[i] == BASIS) {
                total_cost += m_graph.get_weight(i).get_rational() * m_flows[i];
            }
        }

        // m_flows are zero on non-basic edges
        for (unsigned i = 0; i < m_flows.size(); ++i) {
            SASSERT(m_states[i] == BASIS || m_flows[i].is_zero());
        }
        numeral total_balance = numeral::zero();
        for (unsigned i = 0; i < m_potentials.size(); ++i) {
            total_balance += m_balances[i] * m_potentials[i];
        }    
        std::cout << "Total balance: " << total_balance << ", total cost: " << total_cost << std::endl;
        return total_cost == total_balance;
    }

    template<typename Ext>
    std::string network_flow<Ext>::display_spanning_tree() {
            ++m_step;;
            std::ostringstream oss;
            std::string prefix = "T";
            prefix.append(std::to_string(m_step));
            prefix.append("_");
            unsigned root = m_graph.get_num_nodes() - 1;
            for (unsigned i = 0; i < root; ++i) {
                oss << prefix << i << "[shape=circle,label=\"" << prefix << i << " [";
                oss << m_potentials[i] << "/" << m_balances[i] << "]\"];\n";
            }
            oss << prefix << root << "[shape=doublecircle,label=\"" << prefix << root << " [";
            oss << m_potentials[root] << "/" << m_balances[root] << "]\"];\n";
        
            unsigned num_edges = m_graph.get_num_edges();
            for (unsigned i = 0; i < num_edges; ++i) {
                oss << prefix << m_graph.get_source(i) << " -> " << prefix << m_graph.get_target(i);
                if (m_states[i] == BASIS) {
                    oss << "[color=red,penwidth=3.0,label=\"" << m_flows[i] << "/" << m_graph.get_weight(i) << "\"];\n";
                }
                else {
                    oss << "[label=\"" << m_flows[i] << "/" << m_graph.get_weight(i) << "\"];\n";
                }
            }
            oss << std::endl;
            return oss.str();
    }
}

#endif