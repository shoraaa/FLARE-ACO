/**
 * This is an implementation of the Focused Ant Colony Optimization (FACO) for
 * solving large TSP instances as described in the paper:
 *
 * R. Skinderowicz,
 * Improving Ant Colony Optimization efficiency for solving large TSP instances,
 * Applied Soft Computing, 2022, 108653, ISSN 1568-4946,
 * https://doi.org/10.1016/j.asoc.2022.108653.
 *
 * @author: Rafał Skinderowicz (rafal.skinderowicz@us.edu.pl)
*/
#include <cassert>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <ostream>
#include <random>
#include <stdexcept>
#include <vector>
#include <memory>
#include <functional>
#include <filesystem>
#include <queue>


#include <omp.h>

#include "problem_instance.h"
#include "ant.h"
#include "pheromone.h"
#include "local_search.h"
#include "utils.h"
#include "rand.h"
#include "progargs.h"
#include "json.hpp"
#include "logging.h"

#include "env.h"

using namespace std;

namespace fs = std::filesystem;


struct HeuristicData {
    const ProblemInstance &problem_;
    double beta_;

    HeuristicData(const ProblemInstance &instance,
                  double beta)
        : problem_(instance),
          beta_(beta) {
    }

    [[nodiscard]] double get(uint32_t from, uint32_t to) const {
        auto d = problem_.get_distance(from, to);
        return (d > 0) ? 1. / std::pow(d, beta_) : 1;
    }

    [[nodiscard]] uint32_t find_node_with_max_value(uint32_t from, const std::vector<uint32_t> &nodes) const {
        assert(beta_ > 0);

        auto result = from;
        auto min_dist = std::numeric_limits<double>::max();
        for (auto node : nodes) {
            auto dist = problem_.get_distance(from, node);
            if (dist < min_dist) {
                min_dist = dist;
                result = node;
            }
        }
        return result;
    }
};


uint32_t select_max_product_node(
        uint32_t current_node,
        Ant &ant,
        const MatrixPheromone &pheromone,
        const HeuristicData &heuristic) {

    const auto unvisited_count = ant.get_unvisited_count();
    assert( unvisited_count > 0 );

    double max_product = 0;
    const auto &unvisited = ant.get_unvisited_nodes();
    uint32_t chosen_node  = unvisited.front();

    // Nodes in the bucket have non-default pheromone value -- we use
    // a standard method selecting the node with the max. product value
    for (uint32_t i = 0; i < unvisited_count; ++i) {
        auto node = unvisited[i];
        auto prod = pheromone.get(current_node, node) 
                  * heuristic.get(current_node, node);
        if (prod > max_product) {
            chosen_node = node;
            max_product = prod;
        }
    }
    return chosen_node;
}


uint32_t select_max_product_node(
        uint32_t current_node,
        Ant &ant,
        const CandListPheromone &/*pheromone*/,
        const HeuristicData &heuristic) {

    assert( ant.get_unvisited_count() > 0 );
    // We are assuming that all nodes on the cand list of the current_node
    // have been visited and thus we do not need to look for pheromone values
    // as all the other edges have the same - default - value
    return heuristic.find_node_with_max_value(current_node, ant.get_unvisited_nodes());
}

static const uint32_t MaxCandListSize = 32;

struct Limits {
    double min_ = 0;
    double max_ = 0;
};

/**
 * This is based on Eq. 11 from the original MAX-MIN paper:
 *
 * Stützle, Thomas, and Holger H. Hoos. "MAX–MIN ant system." Future generation
 * computer systems 16.8 (2000): 889-914.
 */
Limits calc_trail_limits(uint32_t dimension,
                         uint32_t /*cand_list_size*/,
                         double p_best,
                         double rho,
                         double solution_cost) {
    const auto tau_max = 1 / (solution_cost * (1. - rho));
    const auto cand_count = dimension;
    const auto avg = cand_count / 2.;
    const auto p = pow(p_best, 1. / cand_count);
    const auto tau_min = min(tau_max, tau_max * (1 - p) / ((avg - 1) * p));
    return { tau_min, tau_max };
}


/**
 * This is a modified version of the original trail initialization method
 * used in the FACO
 */
Limits calc_trail_limits_cl(uint32_t /*dimension*/,
                            uint32_t cand_list_size,
                            double p_best,
                            double rho,
                            double solution_cost) {
    const auto tau_max = 1 / (solution_cost * (1. - rho));
    const auto avg = cand_list_size;  // This is far smaller than dimension/2
    const auto p = pow(p_best, 1. / avg);
    const auto tau_min = min(tau_max, tau_max * (1 - p) / ((avg - 1) * p));
    return { tau_min, tau_max };
}


typedef Limits (*calc_trail_limits_fn_t)(uint32_t dimension,
                         uint32_t /*cand_list_size*/,
                         double p_best,
                         double rho,
                         double solution_cost);


template<typename Pheromone_t>
uint32_t select_next_node(const Pheromone_t &pheromone,
                          const HeuristicData &heuristic,
                          const NodeList &nn_list,
                          const vector<double> &nn_product_cache,
                          const NodeList &backup_nn_list,
                          Ant &ant) {
    assert(!ant.route_.empty());

    const auto current_node = ant.get_current_node();
    assert(nn_list.size() <= ::MaxCandListSize);

    // A list of the nearest unvisited neighbors of current_node, i.e. so
    // called "candidates list", or "cl" in short
    uint32_t cl[::MaxCandListSize];
    uint32_t cl_size = 0;

    // In the MMAS the local pheromone evaporation is absent thus for each ant
    // the product of the pheromone trail and the heuristic will be the same
    // and we can pre-load it into nn_product_cache
    auto nn_product_cache_it = nn_product_cache.begin()
                             + static_cast<uint32_t>(current_node * nn_list.size());

    double cl_product_prefix_sums[::MaxCandListSize];
    double cl_products_sum = 0;
    double max_prod = 0;
    uint32_t max_node = current_node;
    for (auto node : nn_list) {
        uint32_t valid = 1 - ant.is_visited(node);
        cl[cl_size] = node;
        auto prod = *nn_product_cache_it * valid;
        cl_products_sum += prod;
        cl_product_prefix_sums[cl_size] = cl_products_sum;
        cl_size += valid;
        ++nn_product_cache_it;
        if (max_prod < prod) {
            max_prod = prod;
            max_node = node;
        }
    }

    uint32_t chosen_node = max_node;

    if (cl_size > 1) { // Select from the closest nodes
        // The following could be done using binary search in O(log(cl_size))
        // time but should not matter for small values of cl_size
        chosen_node = cl[cl_size - 1];
        const auto r = get_rng().next_float() * cl_products_sum;
        for (uint32_t i = 0; i < cl_size; ++i) {
            if (r < cl_product_prefix_sums[i]) {
                chosen_node = cl[i];
                break;
            }
        }
    } else if (cl_size == 0) { // Select from the rest of the unvisited nodes the one with the
                               // maximum product of pheromone and heuristic

        for (auto node : backup_nn_list) {
            if (!ant.is_visited(node)) {
                chosen_node = node;
                break ;
            }
        }
        if (chosen_node == max_node) {  // Still nothing selected
            chosen_node = select_max_product_node(current_node, ant, pheromone, heuristic);
        }
    }
    assert(chosen_node != current_node);
    return chosen_node;
}


void calc_cand_list_heuristic_cache(HeuristicData &heuristic,
                                    uint32_t cl_size,
                                    vector<double> &cache) {
    const auto &problem = heuristic.problem_;
    const auto dimension = problem.dimension_;
    cache.resize(cl_size * dimension);
    for (uint32_t node = 0 ; node < dimension ; ++node) {
        auto cache_it = cache.begin() + node * cl_size;
        for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
            auto value = heuristic.get(node, nn);
            *cache_it++ = value;
        }
    }
}


/**
 * This wraps problem instance and pheromone memory and provides convenient
 * methods to manipulate it.
 */
template<class Impl>
class ACOModel {
protected:
    const ProblemInstance &problem_;
    double p_best_;
    double rho_;
    double rho_min_;
    double deposit_smooth_3_;
    uint32_t cand_list_size_;
public:
    Limits trail_limits_;
    double deposit_smooth_;

    calc_trail_limits_fn_t calc_trail_limits_ = calc_trail_limits;


    ACOModel(const ProblemInstance &problem, const ProgramOptions &options)
        : problem_(problem)
        , p_best_(options.p_best_)
        , rho_(options.rho_)
        , cand_list_size_(options.cand_list_size_)
    {}

    void init(double solution_cost) {
        update_trail_limits(solution_cost);
        static_cast<Impl*>(this)->init_impl();
    }

    void update_trail_limits(double solution_cost) {
        trail_limits_ = calc_trail_limits_(problem_.dimension_, cand_list_size_,
                                           p_best_, rho_, solution_cost);
    }

    void init_trail_limits_smooth() {
        trail_limits_.max_ = 1.0;
        trail_limits_.min_ = 1.0 / cand_list_size_;
        deposit_smooth_ = rho_ * (-trail_limits_.min_ + trail_limits_.max_);
        get_pheromone().init_smooth(rho_ * trail_limits_.min_);

        cout << "trail_limits_.min_: " << trail_limits_.min_ << endl;
    }

    void init_trail_limits_meta(double gain = 0.001) {
        trail_limits_.max_ = 1.0;
        trail_limits_.min_ = 0.0;
        deposit_smooth_ = gain;
    }


    void init_trail_limits_3las() {
        trail_limits_.max_ = 1.0;
        trail_limits_.min_ = 1.0 / pow(cand_list_size_, 2);

        get_pheromone().init_smooth(rho_ * trail_limits_.min_);

        // double mid = (trail_limits_.min_ + trail_limits_.max_) / 2;
        double mid = 1.0 * trail_limits_.max_ / cand_list_size_;
        deposit_smooth_ = rho_ * (-trail_limits_.min_ + mid);
        deposit_smooth_3_ = rho_ * (-mid + trail_limits_.max_);

        cout << "trail_limits_.min_: " << trail_limits_.min_ << endl;
        cout << "trail_limits_.mid_: " << mid << endl;
        cout << "non best deposit: " << rho_ * trail_limits_.min_ << '\n';
        cout << "best route deposit: " << rho_ * mid << '\n';
        cout << "new edges in best deposit: " << rho_ * trail_limits_.max_ << '\n';
    }

    void evaporate_pheromone() {
        get_pheromone().evaporate(1 - rho_, trail_limits_.min_);
    }

    void evaporate_pheromone_smooth() {
        get_pheromone().evaporate_smooth(1 - rho_, trail_limits_.min_);
    }

    decltype(auto) get_pheromone() {
        return static_cast<Impl*>(this)->get_pheromone_impl();
    }

    // Increases amount of pheromone on trails corresponding edges of the
    // given solution (sol). Returns deposited amount. 
    double deposit_pheromone(const Ant &sol) {
        const double deposit = 1.0 / sol.cost_;
        auto prev_node = sol.route_.back();
        auto &pheromone = get_pheromone();
        for (auto node : sol.route_) {
            // The global update of the pheromone trails
            pheromone.increase(prev_node, node, deposit, trail_limits_.max_);
            prev_node = node;
        }
        return deposit;
    }

    void deposit_pheromone_smooth(const Ant &sol) {
        auto prev_node = sol.route_.back();
        auto &pheromone = get_pheromone();
        for (auto node : sol.route_) {
            pheromone.increase(prev_node, node, deposit_smooth_, trail_limits_.max_);
            prev_node = node;
        }
    }

    void deposit_pheromone_smooth(const vector<pair<uint32_t, uint32_t>> &edges) {
        auto &pheromone = get_pheromone();
        for (auto [from, to] : edges) {
            pheromone.increase(from, to, deposit_smooth_, trail_limits_.max_);
        }
    }

    void deposit_pheromone_smooth_3(const vector<pair<uint32_t, uint32_t>> &edges) {
        auto &pheromone = get_pheromone();
        for (auto [from, to] : edges) {
            pheromone.increase(from, to, deposit_smooth_3_, trail_limits_.max_);
        }
    }
};

class MatrixModel : public ACOModel<MatrixModel> {
    std::unique_ptr<MatrixPheromone> pheromone_ = nullptr;
public:

    MatrixModel(const ProblemInstance &problem, const ProgramOptions &options)
        : ACOModel(problem, options)
    {}

    MatrixPheromone &get_pheromone_impl() { return *pheromone_; }

    void init_impl() {
        pheromone_ = std::make_unique<MatrixPheromone>(problem_.dimension_,
                                                       trail_limits_.max_,
                                                       problem_.is_symmetric_);
    }
};

class CandListModel : public ACOModel<CandListModel> {
    std::unique_ptr<CandListPheromone> pheromone_ = nullptr;
public:

    CandListModel(const ProblemInstance &problem, const ProgramOptions &options)
        : ACOModel(problem, options)
    {}

    CandListPheromone &get_pheromone_impl() { return *pheromone_; }

    void init_impl() {
        pheromone_ = std::make_unique<CandListPheromone>(
                problem_.get_nn_lists(cand_list_size_),
                trail_limits_.max_,
                problem_.is_symmetric_);
    }
};

std::pair<std::vector<uint32_t>, double>
build_initial_route(const ProblemInstance &problem, bool use_local_search=false) {
    auto start_node = get_rng().next_uint32(problem.dimension_);
    auto route = problem.build_nn_tour(start_node);
    uint32_t nn_count = 16;
    if (use_local_search) {
        two_opt_nn(problem, route, true, nn_count);
    }
    return { route, problem.calculate_route_length(route) };
}


std::vector<std::vector<uint32_t>> 
par_build_initial_routes(const ProblemInstance &problem,
                         bool use_local_search,
                         uint32_t sol_count=0) {
    uint32_t nn_count = 16;

    if (sol_count == 0) {
        #pragma omp parallel default(none) shared(sol_count)
        #pragma omp master
        sol_count = omp_get_num_procs();
    }

    std::vector<std::vector<uint32_t>> routes(sol_count);

    for (uint32_t i = 0; i < sol_count; ++i) {
        auto start_node = get_rng().next_uint32(problem.dimension_);
        routes[i] = problem.build_nn_tour(start_node);
    }

    if (use_local_search) {
        #pragma omp parallel for default(none) shared(problem, routes, nn_count, sol_count)
        for (uint32_t i = 0; i < sol_count; ++i) {
            two_opt_nn(problem, routes[i], true, nn_count);
            three_opt_nn(problem,  routes[i], /*use_dont_look_bits*/ true, nn_count);
        }
    }
    return routes;
}

/**
 * Runs the MMAS for the specified number of iterations.
 * Returns the best solution (ant).
 */
template<typename Model_t, typename ComputationsLog_t>
std::unique_ptr<Solution> 
run_mmas(const ProblemInstance &problem,
             const ProgramOptions &opt,
             ComputationsLog_t &comp_log) {

    const auto dimension  = problem.dimension_;
    const auto cl_size    = opt.cand_list_size_;
    const auto bl_size    = opt.backup_list_size_;
    const auto ants_count = opt.ants_count_;
    const auto iterations = opt.iterations_;
    const auto use_ls     = opt.local_search_ != 0;

    const auto start_sol = build_initial_route(problem);
    const auto initial_cost = start_sol.second;
    comp_log("initial sol cost", initial_cost);

    Model_t model(problem, opt);
    model.init(initial_cost);
    auto &pheromone = model.get_pheromone();

    HeuristicData heuristic(problem, opt.beta_);

    vector<double> cl_heuristic_cache;
    calc_cand_list_heuristic_cache(heuristic, cl_size, cl_heuristic_cache);

    vector<double> nn_product_cache(dimension * cl_size);

    Ant best_ant;
    best_ant.route_ = start_sol.first;
    best_ant.cost_ = initial_cost;

    vector<Ant> ants(ants_count);
    Ant *iteration_best = nullptr;

    // The following are mainly for raporting purposes
    Trace<ComputationsLog_t, SolutionCost> best_cost_trace(comp_log,
                                                           "best sol cost", iterations, 1, true, 0.1);
    Trace<ComputationsLog_t, double> mean_cost_trace(comp_log, "sol cost mean", iterations, 20);
    Trace<ComputationsLog_t, double> stdev_cost_trace(comp_log, "sol cost stdev", iterations, 20);
    Timer main_timer;

    vector<double> sol_costs(ants_count);

    #pragma omp parallel default(shared)
    {
        for (int32_t iteration = 0 ; iteration < iterations ; ++iteration) {
            #pragma omp barrier
            // Load pheromone * heuristic for each edge connecting nearest
            // neighbors (up to cl_size)
            #pragma omp for schedule(static)
            for (uint32_t node = 0 ; node < dimension ; ++node) {
                auto cache_it = nn_product_cache.begin() + node * cl_size;
                auto heuristic_it = cl_heuristic_cache.begin() + node * cl_size;
                for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
                    *cache_it++ = *heuristic_it++ * pheromone.get(node, nn);
                }
            }

            // Changing schedule from "static" to "dynamic" can speed up
            // computations a bit, however it introduces non-determinism due to
            // threads scheduling. With "static" the computations always follow
            // the same path -- i.e. if we run the program with the same PRNG
            // seed (--seed X) then we get exactly the same results.
            #pragma omp for schedule(static, 1)
            for (uint32_t ant_idx = 0; ant_idx < ants.size(); ++ant_idx) {
                auto &ant = ants[ant_idx];
                ant.initialize(dimension);

                auto start_node = get_rng().next_uint32(dimension);
                ant.visit(start_node);

                while (ant.visited_count_ < dimension) {
                    auto curr = ant.get_current_node();
                    auto next = select_next_node(pheromone, heuristic,
                                                 problem.get_nearest_neighbors(curr, cl_size),
                                                 nn_product_cache,
                                                 problem.get_backup_neighbors(curr, cl_size, bl_size),
                                                 ant);
                    ant.visit(next);
                }
                if (use_ls) {
                    two_opt_nn(problem, ant.route_, true, opt.ls_cand_list_size_);
                }

                ant.cost_ = problem.calculate_route_length(ant.route_);
                sol_costs[ant_idx] = ant.cost_;
            }

            #pragma omp master
            {
                iteration_best = &ants.front();
                for (auto &ant : ants) {
                    if (ant.cost_ < iteration_best->cost_) {
                        iteration_best = &ant;
                    }
                }

                mean_cost_trace.add(round(sample_mean(sol_costs), 1), iteration);
                stdev_cost_trace.add(round(sample_stdev(sol_costs), 1), iteration);

                if (iteration_best->cost_ < best_ant.cost_) {
                    best_ant = *iteration_best;

                    model.update_trail_limits(best_ant.cost_);

                    auto error = problem.calc_relative_error(best_ant.cost_);
                    best_cost_trace.add({ best_ant.cost_, error }, iteration, main_timer());
                }
            }

            // Synchronize threads before pheromone update
            #pragma omp barrier

            model.evaporate_pheromone();

            #pragma omp master
            {
                bool use_best_ant = (get_rng().next_float() < opt.gbest_as_source_prob_);
                auto &update_ant = use_best_ant ? best_ant : *iteration_best;

                model.deposit_pheromone(update_ant);
            }
        }
    }
    return make_unique<Solution>(best_ant.route_, best_ant.cost_);
}

template<typename ComputationsLog_t>
std::unique_ptr<Solution> 
run_focused_aco(const ProblemInstance &problem,
                const ProgramOptions &opt,
                ComputationsLog_t &comp_log) {

    const auto dimension  = problem.dimension_;  
    const auto cl_size    = opt.cand_list_size_;
    const auto bl_size    = opt.backup_list_size_;
    const auto ants_count = opt.ants_count_;
    const auto iterations = opt.iterations_;
    const auto use_ls     = opt.local_search_ != 0;

    Timer start_sol_timer;
    const auto start_routes = par_build_initial_routes(problem, use_ls);
    auto start_sol_count = start_routes.size();
    std::vector<double> start_costs(start_sol_count);

    #pragma omp parallel default(none) shared(start_sol_count, problem, start_costs, start_routes)
    #pragma omp for
    for (size_t i = 0; i < start_sol_count; ++i) {
        start_costs[i] = problem.calculate_route_length(start_routes[i]);
    }
    comp_log("initial solutions build time", start_sol_timer.get_elapsed_seconds());

    auto smallest_pos = std::distance(begin(start_costs),
                                      min_element(begin(start_costs), end(start_costs)));
    auto initial_cost = start_costs[smallest_pos];
    const auto &start_route = start_routes[smallest_pos];
    comp_log("initial sol cost", initial_cost);

    HeuristicData heuristic(problem, opt.beta_);
    vector<double> cl_heuristic_cache;

    cl_heuristic_cache.resize(cl_size * dimension);
    for (uint32_t node = 0 ; node < dimension ; ++node) {
        auto cache_it = cl_heuristic_cache.begin() + node * cl_size;

        for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
            *cache_it++ = heuristic.get(node, nn);
        }
    }

    // Probabilistic model based on pheromone trails:
    CandListModel model(problem, opt);
    // If the LS is on, the differences between pheromone trails should be
    // smaller -- we use calc_trail_limits_cl instead of calc_trail_limits
    model.calc_trail_limits_ = !use_ls ? calc_trail_limits : calc_trail_limits_cl;
    model.init(initial_cost);
    auto &pheromone = model.get_pheromone();
    pheromone.set_all_trails(model.trail_limits_.max_);

    vector<double> nn_product_cache(dimension * cl_size);

    auto best_ant = make_unique<Ant>(start_route, initial_cost);

    vector<Ant> ants(ants_count);
    Ant *iteration_best = nullptr;

    auto source_solution = make_unique<Solution>(start_route, best_ant->cost_);

    // The following are mainly for raporting purposes
    int64_t select_next_node_calls = 0;
    Trace<ComputationsLog_t, SolutionCost> best_cost_trace(comp_log,
                                                           "best sol cost", iterations, 1, true, 1.);
    Trace<ComputationsLog_t, double> select_next_node_calls_trace(comp_log,
                                                                  "mean percent of select next node calls", iterations, 20);
    Trace<ComputationsLog_t, double> mean_cost_trace(comp_log, "sol cost mean", iterations, 20);
    Trace<ComputationsLog_t, double> stdev_cost_trace(comp_log, "sol cost stdev", iterations, 20);
    Timer main_timer;

    vector<double> sol_costs(ants_count);

    double  pher_deposition_time = 0;

    #pragma omp parallel default(shared)
    {
        // Endpoints of new edges (not present in source_route) are inserted
        // into ls_checklist and later used to guide local search
        vector<uint32_t> ls_checklist;
        ls_checklist.reserve(dimension);

        for (int32_t iteration = 0 ; iteration < iterations ; ++iteration) {
            #pragma omp barrier

            // Load pheromone * heuristic for each edge connecting nearest
            // neighbors (up to cl_size)
            #pragma omp for schedule(static)
            for (uint32_t node = 0 ; node < dimension ; ++node) {
                auto cache_it = nn_product_cache.begin() + node * cl_size;
                auto heuristic_it = cl_heuristic_cache.begin() + node * cl_size;
                for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
                    *cache_it++ = *heuristic_it++ * pheromone.get(node, nn);
                }
            }

            #pragma omp master
            select_next_node_calls = 0;

            // Changing schedule from "static" to "dynamic" can speed up
            // computations a bit, however it introduces non-determinism due to
            // threads scheduling. With "static" the computations always follow
            // the same path -- i.e. if we run the program with the same PRNG
            // seed (--seed X) then we get exactly the same results.
            #pragma omp for schedule(static, 1) reduction(+ : select_next_node_calls)
            for (uint32_t ant_idx = 0; ant_idx < ants.size(); ++ant_idx) {
                uint32_t target_new_edges = opt.min_new_edges_;

                auto &ant = ants[ant_idx];
                ant.initialize(dimension);

                auto start_node = get_rng().next_uint32(dimension);
                ant.visit(start_node);

                ls_checklist.clear();
                ls_checklist.push_back(start_node);

                // We are counting edges (undirected) that are not present in
                // the source_route. The factual # of new edges can be +1 as we
                // skip the check for the closing edge (minor optimization).
                uint32_t new_edges = 0;

                while (ant.visited_count_ < dimension) {
                    auto curr = ant.get_current_node();
                    auto next = select_next_node(pheromone, heuristic,
                                                 problem.get_nearest_neighbors(curr, cl_size),
                                                 nn_product_cache,
                                                 problem.get_backup_neighbors(curr, cl_size, bl_size),
                                                 ant);
                    ant.visit(next);

                    ++select_next_node_calls;

                    if (!source_solution->contains_edge(curr, next)) {
                        ++new_edges;
                        // The endpoint (tail) of the new edge should be
                        // checked by the local search
                        ls_checklist.push_back(next);
                    }

                    // If we have enough new edges, we try to copy "old" edges
                    // from the source_route.
                    if (new_edges >= target_new_edges) {
                        // Forward direction, start at { next, succ(next) }
                        auto it = source_solution->get_iterator(next);
                        while (ant.try_visit(it.goto_succ()) ) {
                        }
                        // Backward direction
                        it.goto_pred();  // Reverse .goto_succ() from above
                        while (ant.try_visit(it.goto_pred()) ) {
                        }
                    }
                }
                if (use_ls) {
                    two_opt_nn(problem, ant.route_, ls_checklist, opt.ls_cand_list_size_);
                }

                ant.cost_ = problem.calculate_route_length(ant.route_);
                sol_costs[ant_idx] = ant.cost_;
            }

            #pragma omp master
            {
                iteration_best = &ants.front();
                for (auto &ant : ants) {
                    if (ant.cost_ < iteration_best->cost_) {
                        iteration_best = &ant;
                    }
                }
                if (iteration_best->cost_ < best_ant->cost_) {
                    best_ant->update(iteration_best->route_, iteration_best->cost_);

                    auto error = problem.calc_relative_error(best_ant->cost_);
                    best_cost_trace.add({ best_ant->cost_, error }, iteration, main_timer());

                    model.update_trail_limits(best_ant->cost_);
                }

                auto total_edges = (dimension - 1) * ants_count;
                select_next_node_calls_trace.add(
                        round(100.0 * static_cast<double>(select_next_node_calls) / total_edges, 2),
                        iteration, main_timer());

                mean_cost_trace.add(round(sample_mean(sol_costs), 1), iteration);
                stdev_cost_trace.add(round(sample_stdev(sol_costs), 1), iteration);
            }

            // Synchronize threads before pheromone update
            #pragma omp barrier

            model.evaporate_pheromone();

            #pragma omp master
            {
                bool use_best_ant = (get_rng().next_float() < opt.gbest_as_source_prob_);
                auto &update_ant = use_best_ant ? *best_ant : *iteration_best;

                double start = omp_get_wtime();

                model.deposit_pheromone(update_ant);

                pher_deposition_time += omp_get_wtime() - start;

                // Increase pheromone values on the edges of the new
                // source_solution
                source_solution->update(update_ant.route_, update_ant.cost_);
            }
        }
    }
    comp_log("pher_deposition_time", pher_deposition_time);

    return unique_ptr<Solution>(dynamic_cast<Solution*>(best_ant.release()));
}

struct Mask {
    size_t dimension_;
    std::vector<int8_t> marked_;
    int8_t epoch_ = 0;

    Mask(size_t dimension):
        dimension_(dimension),
        marked_(dimension, 0) {
    }

    /* Clears all bits -- takes O(1) time thanks to incrementing epoch_ counter which
    denotes the (current) "truth" value */
    void clear() { ++epoch_; }

    void set_bit(uint32_t index) {
        assert(index < dimension_);
        marked_[index] = epoch_;
    }

    bool is_set(uint32_t index) const {
        assert(index < dimension_);
        return marked_[index] == epoch_;
    }
};

/* A specialized version of select_next_node that does not require *struct Ant* */
template<typename Pheromone_t, typename Mask_t>
uint32_t select_next_node(const Pheromone_t &/*pheromone*/,
                          const HeuristicData &heuristic,
                          const NodeList &nn_list,
                          const vector<double> &nn_product_cache,
                          const NodeList &backup_nn_list,
                          const uint32_t current_node,
                          Mask_t &visited) {

    assert(nn_list.size() <= ::MaxCandListSize);

    // A list of the nearest unvisited neighbors of current_node, i.e. so
    // called "candidates list", or "cl" in short
    uint32_t cl[::MaxCandListSize];
    uint32_t cl_size = 0;

    // In the MMAS the local pheromone evaporation is absent thus for each ant
    // the product of the pheromone trail and the heuristic will be the same
    // and we can pre-load it into nn_product_cache
    auto nn_product_cache_it = nn_product_cache.begin()
                             + static_cast<uint32_t>(current_node * nn_list.size());

    double cl_product_prefix_sums[::MaxCandListSize];
    double cl_products_sum = 0;
    double max_prod = 0;
    uint32_t max_node = current_node;
    for (auto node : nn_list) {
        uint32_t valid = 1 - visited.is_set(node);
        cl[cl_size] = node;
        auto prod = *nn_product_cache_it * valid;
        cl_products_sum += prod;
        cl_product_prefix_sums[cl_size] = cl_products_sum;
        cl_size += valid;
        ++nn_product_cache_it;
        if (max_prod < prod) {
            max_prod = prod;
            max_node = node;
        }
    }

    uint32_t chosen_node = max_node;

    if (cl_size > 1) { // Select from the closest nodes
        // The following could be done using binary search in O(log(cl_size))
        // time but should not matter for small values of cl_size
        chosen_node = cl[cl_size - 1];
        const auto r = get_rng().next_float() * cl_products_sum;
        for (uint32_t i = 0; i < cl_size; ++i) {
            if (r < cl_product_prefix_sums[i]) {
                chosen_node = cl[i];
                break;
            }
        }
    } else if (cl_size == 0) { // Select from the rest of the unvisited nodes the one with the
                               // maximum product of pheromone and heuristic

        // cout << "MISS\n"; // we shouldnt get here since no node should be mark visited

        for (auto node : backup_nn_list) {
            if (!visited.is_set(node)) {
                chosen_node = node;
                break ;
            }
        }
        if (chosen_node == max_node) {  // Still nothing selected
            const auto &problem = heuristic.problem_;
            auto min_dist = std::numeric_limits<double>::max();

            for (uint32_t node = 0; node < problem.dimension_; ++node) {

                if (visited.is_set(node)) { continue ; }

                auto dist = problem.get_distance(current_node, node);
                if (dist < min_dist) {
                    min_dist = dist;
                    chosen_node = node;
                }
            }
        }
    }
    assert(chosen_node != current_node);
    return chosen_node;
}


std::vector<uint32_t> ga_eax_indi_to_route(const TIndi& indi, uint32_t Ncity) {
    std::vector<uint32_t> route(Ncity);
    int curr, next, pre, st, count;

    count = 0;
    pre = -1;
    curr = 0;
    st = 0;
    while( 1 )
    {
        route[ count++ ] = curr;

        if( count > Ncity ){
            printf( "Invalid\n" );
            abort();
        } 
    
        if( indi.fLink[ curr ][ 0 ] == pre )
            next = indi.fLink[ curr ][ 1 ];
        else 
            next = indi.fLink[ curr ][ 0 ];

        pre = curr;
        curr = next;
        if( curr == st )
        break;
    }

    return route;
}

class Route {
public:
    using CostFunction = std::function<double (uint32_t, uint32_t)>;


    Route(std::vector<uint32_t> route, CostFunction fn)
        : route_(route),
          cost_fn_(fn)
    {
        positions_.resize(route.size());
        uint32_t pos = 0;
        for (auto node : route) {
            positions_[node] = pos++;
        }
    }

    /**
    Relocates node so that it directly follows the target node.

    For example, if n denotes the relocated node and t is the target, then:
        (1 2 3 t 5 6 n 7 8) => (1 2 3 t n 5 6 7 8)
    or
        (1 2 3 n 5 6 t 7 8) => (1 2 3 5 6 t n 7 8)
    
    The cost of the route is updated.
    Time complexity is O(n).
    */
    void relocate_node(uint32_t target, uint32_t node) {
        assert(node != target);
        assert(node < route_.size());
        assert(target < route_.size());

        if (get_succ(target) == node) { return ; }

        const auto node_pos = positions_[node];
        const auto target_pos = positions_[target];
        const auto len = route_.size();

        const auto node_pred = get_pred(node);
        const auto node_succ = get_succ(node);
        const auto target_succ = get_succ(target);

        if (target_pos < node_pos) {  // Case 1.
            // 1 2 3 t 5 6 n 7 8 =>
            // 1 2 3 t n 5 6 7 8
            auto beg = route_.rbegin() + len - 1 - node_pos;
            auto end = route_.rbegin() + len - 1 - target_pos;

            std::rotate(beg, beg + 1, end);

            for (auto i = target_pos; i <= node_pos; ++i) {
                positions_[route_[i]] = i;
            }
        } else { // Case 2.
            // 1 2 3 n 5 6 t 7 8 =>
            // 1 2 3 5 6 t n 7 8
            auto beg = route_.begin() + node_pos;
            auto end = route_.begin() + target_pos + 1;
            std::rotate(beg, beg + 1, end);

            for (auto i = node_pos; i <= target_pos; ++i) {
                positions_[route_[i]] = i;
            }
        }

        assert(get_succ(target) == node);

        // We are removing these edges:
        cost_ += - cost_fn_(node_pred, node)
                 - cost_fn_(node, node_succ)
                 - cost_fn_(target, target_succ)
                 + cost_fn_(node_pred, node_succ)
                 + cost_fn_(target, node)
                 + cost_fn_(node, target_succ);
    }

    size_t size() const { return route_.size(); }

    uint32_t operator[](uint32_t index) const {
        assert(index < route_.size());
        return route_[index]; 
    }

    uint32_t get_succ(uint32_t node) const {
        assert(node < route_.size());
        auto pos = positions_.at(node);
        return (pos + 1 == route_.size())
             ? route_[0]
             : route_[pos + 1];
    }

    uint32_t get_pred(uint32_t node) const {
        assert(node < route_.size());
        auto pos = positions_.at(node);
        return pos == 0
             ? route_.back()
             : route_[pos - 1];
    }

    bool contains_edge(uint32_t a, uint32_t b) const {
        return b == get_succ(a) || b == get_pred(a);
    }

    bool contains_directed_edge(uint32_t a, uint32_t b) const {
        return b == get_succ(a);
    }

    /*
    * This performs a 2-opt move by flipping a section of the route.  The
    * boundaries of the section are given by first and last, i.e.  it reverses
    * the segment [start_node, ..., end_node), however it may happen that the section is
    * very long compared to the remaining part of the route. In such case, the
    * remaining part is flipped, to speed things up as the result of such flip
    * results in equivalent solution.
    */
    int32_t flip_route_section(int32_t start_node, int32_t end_node) {
        auto first = positions_[start_node];
        auto last = positions_[end_node];

        if (first > last) {
            std::swap(first, last);
        }

        const auto length = static_cast<int32_t>(route_.size());
        const int32_t segment_length = last - first;
        const int32_t remaining_length = length - segment_length;

        if (segment_length <= remaining_length) {  // Reverse the specified segment
            std::reverse(route_.begin() + first, route_.begin() + last);

            for (auto k = first; k < last; ++k) {
                positions_[ route_[k] ] = k;
            }
            return first;
        } else {  // Reverse the rest of the route, leave the segment intact
            first = (first > 0) ? first - 1 : length - 1;
            last = last % length;
            std::swap(first, last);
            int32_t l = first;
            int32_t r = last;
            int32_t i = 0;
            int32_t j = length - first + last + 1;
            while(i++ < j--) {
                std::swap(route_[l], route_[r]);
                positions_[route_[l]] = l;
                positions_[route_[r]] = r;
                l = (l+1) % length;
                r = (r > 0) ? r - 1 : length - 1;
            }
        }
        return 0;
    }

    double get_dist_to_succ(uint32_t node) {
        return cost_fn_(node, get_succ(node));
    }

    /**
     * This impl. of the 2-opt heuristic uses a queue of nodes to check for an
     * improving move, i.e. checklist. This is useful to speed up computations
     * if the route was 2-optimal but a few new edges were introduced -- endpoints
     * of the new edges should be inserted into checklist.
     */
    void two_opt_nn(const ProblemInstance &instance,
                    std::vector<uint32_t> &checklist,
                    uint32_t nn_list_size) {

        // We assume symmetry so that the order of the nodes does not matter
        assert(instance.is_symmetric_);

        // Setting maximum number of allowed route changes prevents very long-running times
        // for very hard to solve TSP instances.
        const uint32_t MaxChanges = size();
        uint32_t changes_count = 0;

        double cost_change = 0;

        size_t checklist_pos_pos = 0;
        while (checklist_pos_pos < checklist.size() && changes_count < MaxChanges) {
            auto a = checklist[checklist_pos_pos++];
            assert(a < route_.size());

            auto a_next = get_succ(a);
            auto a_prev = get_pred(a);

            auto dist_a_to_next = get_dist_to_succ(a);// instance.get_distance(a, a_next);
            auto dist_a_to_prev = get_dist_to_succ(a_prev);

            double max_diff = -1;
            std::array<uint32_t, 4> move;

            const auto &nn_list = instance.get_nearest_neighbors(a, nn_list_size);

            for (auto b : nn_list) {
                auto dist_ab = instance.get_distance(a, b);
                if (dist_a_to_next > dist_ab) {
                    // We rotate the section between a and b_next so that
                    // two new (undirected) edges are created: { a, b } and { a_next, b_next }
                    //
                    // a -> a_next ... b -> b_next
                    // a -> b ... a_next -> b_next
                    //
                    // or
                    //
                    // b -> b_next ... a -> a_next
                    // b -> a ... b_next -> a_next
                    auto b_next = get_succ(b);

                    auto diff = dist_a_to_next
                            + get_dist_to_succ(b) //instance.get_distance(b, b_next)
                            - dist_ab
                            - instance.get_distance(a_next, b_next);

                    if (diff > max_diff) {
                        move = { a_next, b_next, a, b };
                        max_diff = diff;
                    }
                } else {
                    break ;
                }
            }

            for (auto b : nn_list) {
                auto dist_ab = instance.get_distance(a, b);
                if (dist_a_to_prev > dist_ab) {
                    // We rotate the section between a_prev and b so that
                    // two new (undirected) edges are created: { a, b } and { a_prev, b_prev }
                    //
                    // a_prev -> a ... b_prev -> b
                    // a_prev -> b_prev ... a -> b
                    //
                    // or
                    //
                    // b_prev -> b ... a_prev -> a
                    // b_prev -> a_prev ... b -> a
                    auto b_prev = get_pred(b);

                    auto diff = dist_a_to_prev
                            + get_dist_to_succ(b_prev)
                            - dist_ab
                            - instance.get_distance(a_prev, b_prev);

                    if (diff > max_diff) {
                        move = { a, b, a_prev, b_prev };
                        max_diff = diff;
                    }
                } else {
                    break ;
                }
            }


            if (max_diff > 0) {
                flip_route_section(move[0], move[1]);

                for (auto x : move) {
                    if (std::find(checklist.begin() + static_cast<int32_t>(checklist_pos_pos),
                                checklist.end(), x) == checklist.end()) {
                        checklist.push_back(x);
                    }
                }
                ++changes_count;
                cost_change -= max_diff;
            }
        }
        assert(instance.is_route_valid(route_));
        cost_ += cost_change;
    }


    /**
     * This use use GA-EAX to solve a sub-problem of the original problem, that is 
     * fixing the route between the nodes in the checklist. The checklist is a list of
     * nodes that were recently added to the route and may have caused the route to be
     * not GA-EAX optimal anymore.
     */
    void ga_eax(const ProblemInstance& instance, std::vector<uint32_t>& checklist, TEnvironment* gEnv, Bitmask& fixed_nodes, vector<uint32_t>& index, std::vector<std::tuple<uint32_t, uint32_t, int64_t>>& fixed_edges, std::vector<Vec2d>& coords, std::vector<std::pair<uint32_t, uint32_t>>& fixed_edges_indices) {
        if (checklist.empty()) return;
        // Mark fixed nodes
        fixed_nodes.clear();
        for (auto node : checklist) {
            fixed_nodes.set_bit(node);
        }

        // Expand checklist to include predecessors
        size_t s = checklist.size();
        for (int i = 0; i < s; ++i) {
            checklist.push_back(get_pred(checklist[i]));
        }
        std::sort(checklist.begin(), checklist.end());
        checklist.erase(std::unique(checklist.begin(), checklist.end()), checklist.end());

        // if (checklist.size() < 50) return;
        // cout << "GA-EAX: " << checklist.size() << endl;

        // Compress contiguous segments into single edges
        fixed_edges.clear();
        uint32_t start_node = checklist[0], curr = start_node;
        double current_edges_weight = 0, total_edges_weight = 0;

        for (size_t i = 0; i < route_.size(); ++i) {
            uint32_t next = get_succ(curr);
            double weight = get_dist_to_succ(curr);
            
            if (fixed_nodes.is_set(next)) {
                if (current_edges_weight > 0) {
                    fixed_edges.emplace_back(start_node, curr, current_edges_weight);
                    total_edges_weight += current_edges_weight;
                    current_edges_weight = 0;
                }
                start_node = next;
            } else {
                current_edges_weight += weight;
            }
            curr = next;
        }

        if (current_edges_weight > 0) {
            fixed_edges.emplace_back(start_node, curr, current_edges_weight);
            total_edges_weight += current_edges_weight;
        }

        // Convert checklist to coordinates
        coords.clear();
        for (auto node : checklist) {
            coords.emplace_back(instance.coords_[node]);
        }

        for (uint32_t i = 0; i < checklist.size(); ++i) {
            fixed_nodes.set_bit(checklist[i]);
            index[checklist[i]] = i;
        }

        // Convert fixed_edges indices to checklist indices
        fixed_edges_indices.clear();
        for (auto [a, b, w] : fixed_edges) {
            int idx_a = lower_bound(checklist.begin(), checklist.end(), a) - checklist.begin();
            int idx_b = lower_bound(checklist.begin(), checklist.end(), b) - checklist.begin();
            fixed_edges_indices.emplace_back(idx_a, idx_b);
        }

        gEnv->fEvaluator->SetInstance(checklist, coords, fixed_edges_indices, cost_fn_);
        gEnv->Redefine();
        gEnv->tKopt->SetInvNearList();
        gEnv->DoIt();

        // // // Convert GA-EAX result to route
        // // cout << "Converting GA-EAX result to route" << endl;
        auto& result = gEnv->tBest;

        // Map new route indices to original node indices
        std::vector<uint32_t> new_route_mapped;
        new_route_mapped.reserve(checklist.size());
        for (auto x : ga_eax_indi_to_route(result, checklist.size())) new_route_mapped.push_back(checklist[x]);

        // Compute cost
        int64_t cost = result.fEvaluationValue + fixed_edges.size() * 1e9 + total_edges_weight;

        // route_.clear();
        // Construct final route
        std::vector<uint32_t> route, sub_route;
        route.reserve(instance.dimension_);
        sub_route.reserve(instance.dimension_);

        for (size_t i = 0; i < new_route_mapped.size(); ++i) {
            uint32_t x = new_route_mapped[i];
            uint32_t y = new_route_mapped[(i + 1) % new_route_mapped.size()];
            
            route.push_back(x);
            
            sub_route.clear();
            for (auto& [a, b, w] : fixed_edges) {
                if ((a == x && b == y) || (a == y && b == x)) {
                    uint32_t next = get_succ(a);
                    while (next != b) {
                        sub_route.push_back(next);
                        next = get_succ(next);
                    }
                    if (b == x) std::reverse(sub_route.begin(), sub_route.end());
                    break;
                }
            }
            
            route.insert(route.end(), sub_route.begin(), sub_route.end());
        }
        // cout << "GA-EAX result converted to route" << endl;

        // check if all fixed edges are in the new route
        // int fixed_edges_count = 0;
        // for (auto [a, b, _] : fixed_edges) {
        //     int idx_a = find(new_route_mapped.begin(), new_route_mapped.end(), a) - new_route_mapped.begin();
        //     int idx_b = find(new_route_mapped.begin(), new_route_mapped.end(), b) - new_route_mapped.begin();
        //     if (abs(idx_a - idx_b) != 1 && abs(idx_a - idx_b) != new_route_mapped.size() - 1) {
        //         cout << "Error: fixed edge not in new route" << endl;
        //         cout << a << " " << b << " " << _ << endl;
        //     } else {
        //         ++fixed_edges_count;
        //     }
        // }

        // if (fixed_edges_count != fixed_edges.size()) {
        //     cout << "Error: not all fixed edges are in the new route" << endl;
        //     cout << "Fixed edges: " << fixed_edges_count << " " << fixed_edges.size() << endl;
        //     abort();
        // }

        // if (route.size() != route_.size()) {
        //     cout << "Route size mismatch: " << route.size() << " != " << route_.size() << endl;
        //     abort();
        // }

        // if (cost != instance.calculate_route_length(route)) {
        //     cout << "Route cost mismatch: " << cost << " != " << instance.calculate_route_length(route) << endl;
        //     abort();
        // }

        // cout << "cost from " << cost_ << " to " << cost << endl;

        route_ = route;
        cost_ = cost;
    }

    /**
     * This use use GA-EAX to solve a sub-problem of the original problem, that is 
     * fixing the route between the nodes in the checklist. The checklist is a list of
     * nodes that were recently added to the route and may have caused the route to be
     * not GA-EAX optimal anymore.
     */
    void ga_eax_fixed_edges(const ProblemInstance& instance, std::vector<uint32_t>& deleted_edges, TEnvironment* gEnv, Bitmask& fixed_nodes, vector<uint32_t>& index, std::vector<std::tuple<uint32_t, uint32_t, int64_t>>& fixed_edges, std::vector<Vec2d>& coords, std::vector<std::pair<uint32_t, uint32_t>>& fixed_edges_indices) {

        vector<uint32_t> checklist;
        // Compress contiguous segments into single edges
        fixed_edges.clear();
        uint32_t start_node = route_[0], curr = start_node;
        double current_edges_weight = 0, total_edges_weight = 0;

        for (size_t i = 0; i < route_.size(); ++i) {
            uint32_t next = get_succ(curr);
            double weight = get_dist_to_succ(curr);

            if (deleted_edges[curr]) {
                checklist.push_back(curr);
                checklist.push_back(next);
                if (current_edges_weight > 0) {
                    fixed_edges.emplace_back(start_node, curr, current_edges_weight);
                    total_edges_weight += current_edges_weight;
                    current_edges_weight = 0;
                }
                start_node = next;
            } else {
                current_edges_weight += weight;
                if (i == route_.size() - 1) {
                    if (fixed_edges.empty()) {
                        cout << "Error: all edges are fixed" << endl;
                        abort();
                    } 

                    // if first node belong to a fixed edge, modify that edges
                    // otherwise, add a new fixed edge
                    total_edges_weight += current_edges_weight;
                    auto [a, b, w] = fixed_edges[0];

                    if (a == next) {
                        fixed_edges[0] = {start_node, b, w + current_edges_weight};
                    } else {
                        fixed_edges.emplace_back(start_node, next, current_edges_weight);
                    }
                }
            }

            curr = next;
        }

        std::sort(checklist.begin(), checklist.end());
        checklist.erase(std::unique(checklist.begin(), checklist.end()), checklist.end());

        // Convert checklist to coordinates
        coords.clear();
        for (auto node : checklist) {
            coords.emplace_back(instance.coords_[node]);
        }

        // Convert fixed_edges indices to checklist indices
        fixed_edges_indices.clear();
        for (auto [a, b, w] : fixed_edges) {
            int idx_a = lower_bound(checklist.begin(), checklist.end(), a) - checklist.begin();
            int idx_b = lower_bound(checklist.begin(), checklist.end(), b) - checklist.begin();
            fixed_edges_indices.emplace_back(idx_a, idx_b);
        }

        gEnv->fEvaluator->SetInstance(checklist, coords, fixed_edges_indices, cost_fn_);
        gEnv->Redefine();
        gEnv->tKopt->SetInvNearList();
        gEnv->DoIt();

        // // // Convert GA-EAX result to route
        // // cout << "Converting GA-EAX result to route" << endl;
        auto& result = gEnv->tBest;

        // Map new route indices to original node indices
        std::vector<uint32_t> new_route_mapped;
        new_route_mapped.reserve(checklist.size());
        for (auto x : ga_eax_indi_to_route(result, checklist.size())) new_route_mapped.push_back(checklist[x]);

        // Compute cost
        int64_t cost = result.fEvaluationValue + fixed_edges.size() * 1e9 + total_edges_weight;

        // Construct final route
        std::vector<uint32_t> route, sub_route;
        route.reserve(instance.dimension_);
        sub_route.reserve(instance.dimension_);

        for (size_t i = 0; i < new_route_mapped.size(); ++i) {
            uint32_t x = new_route_mapped[i];
            uint32_t y = new_route_mapped[(i + 1) % new_route_mapped.size()];
            
            route.push_back(x);
            
            sub_route.clear();
            for (auto& [a, b, w] : fixed_edges) {
                if ((a == x && b == y) || (a == y && b == x)) {
                    uint32_t next = get_succ(a);
                    while (next != b) {
                        sub_route.push_back(next);
                        next = get_succ(next);
                    }
                    if (b == x) std::reverse(sub_route.begin(), sub_route.end());
                }
            }
            
            route.insert(route.end(), sub_route.begin(), sub_route.end());
        }
        // cout << "GA-EAX result converted to route" << endl;

        // check if all fixed edges are in the new route
        int fixed_edges_count = 0;
        for (auto [a, b, _] : fixed_edges) {
            int idx_a = find(new_route_mapped.begin(), new_route_mapped.end(), a) - new_route_mapped.begin();
            int idx_b = find(new_route_mapped.begin(), new_route_mapped.end(), b) - new_route_mapped.begin();
            if (abs(idx_a - idx_b) != 1 && abs(idx_a - idx_b) != new_route_mapped.size() - 1) {
                cout << "Error: fixed edge not in new route" << endl;
                cout << a << " " << b << " " << _ << endl;
            } else {
                ++fixed_edges_count;
            }
        }

        // if (fixed_edges_count != fixed_edges.size()) {
        //     cout << "Error: not all fixed edges are in the new route" << endl;
        //     cout << "Fixed edges: " << fixed_edges_count << " " << fixed_edges.size() << endl;
        //     abort();
        // }

        // if (route.size() != route_.size()) {
        //     cout << "Route size mismatch: " << route.size() << " != " << route_.size() << endl;
        //     abort();
        // }

        // if (cost != instance.calculate_route_length(route)) {
        //     cout << "Route cost mismatch: " << cost << " != " << instance.calculate_route_length(route) << endl;
        //     abort();
        // }

        // cout << "cost from " << cost_ << " to " << cost << endl;

        route_ = route;
        cost_ = cost;
    }

    std::vector<uint32_t> route_;
    std::vector<uint32_t> positions_;  // node to index in route_ mapping
    double cost_ = 0;
    CostFunction cost_fn_;
};


class LinkedRoute {
public:

    using CostFunction = std::function<double (uint32_t, uint32_t)>;

    std::vector<uint32_t> succ_, pred_;
    double cost_ = 0;
    CostFunction cost_fn_;
    uint32_t start_node_ = 0;

    LinkedRoute() {}

    LinkedRoute(Route route) {
        cost_ = route.cost_;
        start_node_ = route[0];
        cost_fn_ = route.cost_fn_;
        succ_.resize(route.size());
        pred_.resize(route.size());
        for (uint32_t i = 0; i < route.size(); ++i) {
            auto node = route[i];
            succ_[node] = route[(i + 1) % route.size()];
            pred_[node] = route[(i + route.size() - 1) % route.size()];
        }
    }

    Route to_route() const {
        std::vector<uint32_t> route;
        route.reserve(succ_.size());
        auto node = start_node_;
        for (size_t i = 0; i < succ_.size(); ++i) {
            route.push_back(node);
            node = succ_[node];
        }
        auto result = Route(route, cost_fn_);
        result.cost_ = cost_;
        return result;
    }

    /**
    Relocates node so that it directly follows the target node.

    For example, if n denotes the relocated node and t is the target, then:
        (1 2 3 t 5 6 n 7 8) => (1 2 3 t n 5 6 7 8)
    or
        (1 2 3 n 5 6 t 7 8) => (1 2 3 5 6 t n 7 8)
    
    The cost of the route is updated.
    Time complexity is O(n) for normal route. In this impl. it is O(1).
    */
    void relocate_node(uint32_t target, uint32_t node) {
        assert(node != target);
        assert(node < route_.size());
        assert(target < route_.size());

        if (get_succ(target) == node) { return ; }

        const auto node_pred = get_pred(node);
        const auto node_succ = get_succ(node);
        const auto target_succ = get_succ(target);

        succ_[node] = target_succ;
        pred_[target_succ] = node;

        succ_[target] = node;
        pred_[node] = target;

        succ_[node_pred] = node_succ;
        pred_[node_succ] = node_pred;

        // We are removing these edges:
        cost_ += - cost_fn_(node_pred, node)
                 - cost_fn_(node, node_succ)
                 - cost_fn_(target, target_succ)
                 + cost_fn_(node_pred, node_succ)
                 + cost_fn_(target, node)
                 + cost_fn_(node, target_succ);
    }


    size_t size() const { return succ_.size(); }

    uint32_t get_succ(uint32_t node) const {
        assert(node < succ_.size());
        return succ_[node];
    }

    uint32_t get_pred(uint32_t node) const {
        assert(node < pred_.size());
        return pred_[node];
    }

    bool contains_edge(uint32_t a, uint32_t b) const {
        return b == get_succ(a) || b == get_pred(a);
    }

    bool contains_directed_edge(uint32_t a, uint32_t b) const {
        return b == get_succ(a);
    }

};



template<typename T>
bool contains(const std::vector<T> &vec, T value) {
    return std::find(vec.begin(), vec.end(), value) != vec.end();
}

/**
 * Returs # of edges that are in route a but not in route b.
 */
template<typename Route>
int32_t count_diff_edges(const Route &a, const Route &b) {
    assert(a.size() >= 1);

    int32_t count = 0;
    const auto n = a.size();
    auto prev = a[n - 1];
    for (size_t i = 0; i < n; ++i) {
        const auto curr = a[i];
        if (!b.contains_edge(prev, curr)) {
            ++count;
        }
        prev = curr;
    }
    return count;
}

/**
 * Returs edges that are in route a but not in route b.
 */
template<typename Route>
void produce_diff_edges(const Route &a, const Route &b, vector<pair<uint32_t, uint32_t>> &diff) {
    assert(a.size() >= 1);

    const auto& route = a.route_;
    const auto n = route.size();
    auto prev = route[n - 1];
    for (size_t i = 0; i < n; ++i) {
        const auto curr = route[i];
        if (!b.contains_edge(prev, curr)) {
            diff.push_back({prev, curr});
        }
        prev = curr;
    }
}


void route_diff_to_svg(const ProblemInstance &instance,
                  const Route &a,
                  const Route &b,
                  const std::string &path) {
    using namespace std;

    assert(a.size() == b.size());

    if (instance.coords_.empty()) {  // No coords., no picture
        return ;
    }

    ofstream out(path);
    if (out.is_open()) {
        auto p = instance.coords_.at(0);
        auto min_x = p.x_;
        auto max_x = min_x;
        auto min_y = p.y_;
        auto max_y = min_y;

        for (auto &c : instance.coords_) {
            min_x = min(min_x, c.x_);
            max_x = max(max_x, c.x_);
            min_y = min(min_y, c.y_);
            max_y = max(max_y, c.y_);
        }

        // We are scaling the image so that the width equals 1000.0,
        // and the height is scaled proportionally (keeping the original
        // ratio).
        auto width  = max_x - min_x;
        auto height = max_y - min_y;

        auto hw_ratio = height / width;

        auto svg_width  = 1000.0;
        auto svg_height = svg_width * hw_ratio;

        out << "<?xml version=\"1.0\" standalone=\"no\"?>\n"
            << "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n"
            << "<svg version=\"1.1\""
            << " viewBox=\"" << 0
            << " " << 0
            << " " << svg_width
            << " " << svg_height
            << "\">\n"
            << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";

        out << "<defs>\n"
            << R"(<marker id="triangle" viewBox="0 0 10 10" refX="1" refY="5" markerUnits="strokeWidth" markerWidth="10" markerHeight="10" orient="auto">)"
            << R"(<path d="M 0 0 L 10 5 L 0 10 z" fill="#f00" />)"
            << "</marker>"
            << "</defs>\n";

        const auto len = a.size();
        auto prev_id = a[len - 1];
        p = instance.coords_.at(prev_id);

        auto x = p.x_;
        auto y = p.y_;

        auto scale_x = [=](double x) { return svg_width - (x - min_x) / width * svg_width; };
        auto scale_y = [=](double y) { return (y - min_y) / height * svg_height; };

        x = svg_width - (x - min_x) / width * svg_width;
        y = (y - min_y) / height * svg_height;

        out << R"(<polyline fill="none" stroke="black" stroke-width="0.5" points=")"
            << x << "," << y;

        for (size_t i = 0; i < len; ++i) {
            auto id = a[i];
            p = instance.coords_.at(id);
            x = p.x_;
            y = p.y_;
            x = svg_width - (x - min_x) / width * svg_width;
            y = (y - min_y) / height * svg_height;
            out << " " << x << "," << y;
        }
        out << "\"/>\n";

        // Plot difference btw. a & b:
        auto prev = a[len - 1];
        for (size_t i = 0; i < len; ++i) {
            const auto curr = a[i];
            if (!b.contains_edge(prev, curr)) {
                auto p1 = instance.coords_.at(prev);
                auto p2 = instance.coords_.at(curr);

                out << R"(<polyline fill="none" stroke="white" stroke-width="0.9" points=")"
                    << scale_x(p1.x_) << "," << scale_y(p1.y_) 
                    << " " 
                    << scale_x(p2.x_) << "," << scale_y(p2.y_)
                    << "\"/>\n";

                out << R"(<polyline fill="none" stroke="black" stroke-width="0.5" points=")"
                    << scale_x(p1.x_) << "," << scale_y(p1.y_) 
                    << " " 
                    << scale_x(p2.x_) << "," << scale_y(p2.y_)
                    << "\" stroke-dasharray=\"4\" />\n";
            }
            prev = curr;
        }

        prev = b[len - 1];
        for (size_t i = 0; i < len; ++i) {
            const auto curr = b[i];
            if (!a.contains_edge(prev, curr)) {
                auto p1 = instance.coords_.at(prev);
                auto p2 = instance.coords_.at(curr);

                out << R"(<polyline fill="none" stroke="red" stroke-width="0.5" points=")"
                    << scale_x(p1.x_) << "," << scale_y(p1.y_) 
                    << " " 
                    << scale_x(p2.x_) << "," << scale_y(p2.y_)
                    << "\"/>\n";
            }
            prev = curr;
        }

        out << "</svg>";

        out.close();
    }
}


template<typename ComputationsLog_t>
std::unique_ptr<Solution> 
run_mfaco(const ProblemInstance &problem,
                const ProgramOptions &opt,
                ComputationsLog_t &comp_log) {

    const auto dimension  = problem.dimension_;  
    const auto cl_size    = opt.cand_list_size_;
    const auto bl_size    = opt.backup_list_size_;
    const auto ants_count = 256; //opt.ants_count_;
    const auto iterations = 5000; //opt.iterations_;
    const auto use_ls     = opt.local_search_ != 0;

    Timer start_sol_timer;
    const auto start_routes = par_build_initial_routes(problem, use_ls);
    auto start_sol_count = start_routes.size();
    std::vector<double> start_costs(start_sol_count);

    #pragma omp parallel default(none) shared(start_sol_count, problem, start_costs, start_routes)
    #pragma omp for
    for (size_t i = 0; i < start_sol_count; ++i) {
        start_costs[i] = problem.calculate_route_length(start_routes[i]);
    }
    comp_log("initial solutions build time", start_sol_timer.get_elapsed_seconds());

    auto smallest_pos = std::distance(begin(start_costs),
                                      min_element(begin(start_costs), end(start_costs)));
    auto initial_cost = start_costs[smallest_pos];
    const auto &start_route = start_routes[smallest_pos];
    comp_log("initial sol cost", initial_cost);

    HeuristicData heuristic(problem, opt.beta_);
    vector<double> cl_heuristic_cache;

    cl_heuristic_cache.resize(cl_size * dimension);
    for (uint32_t node = 0 ; node < dimension ; ++node) {
        auto cache_it = cl_heuristic_cache.begin() + node * cl_size;

        for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
            *cache_it++ = heuristic.get(node, nn);
        }
    }

    // Probabilistic model based on pheromone trails:
    CandListModel model(problem, opt);
    // If the LS is on, the differences between pheromone trails should be
    // smaller -- we use calc_trail_limits_cl instead of calc_trail_limits
    model.calc_trail_limits_ = !use_ls ? calc_trail_limits : calc_trail_limits_cl;
    model.init(initial_cost);
    auto &pheromone = model.get_pheromone();
    pheromone.set_all_trails(model.trail_limits_.max_);

    vector<double> nn_product_cache(dimension * cl_size);

    auto best_ant = make_unique<Ant>(start_route, initial_cost);

    vector<Ant> ants(ants_count);
    for (auto &ant : ants) {
        ant = *best_ant;
    }

    Ant *iteration_best = nullptr;

    auto source_solution = make_unique<Solution>(start_route, best_ant->cost_);

    // The following are mainly for raporting purposes
    Trace<ComputationsLog_t, SolutionCost> best_cost_trace(comp_log,
                                                           "best sol cost", iterations, 1, true, 1.);
    Trace<ComputationsLog_t, double> mean_cost_trace(comp_log, "sol cost mean", iterations, 20);
    Trace<ComputationsLog_t, double> stdev_cost_trace(comp_log, "sol cost stdev", iterations, 20);
    Timer main_timer;

    vector<double> sol_costs(ants_count);

    double  pher_deposition_time = 0;
    double average_visited_count = 0;
    double construction_time = 0;
    double relocation_time = 0;
    int32_t ant_sol_updates = 0;
    int32_t local_source_sol_updates = 0;
    int32_t total_new_edges = 0;

    #pragma omp parallel default(shared)
    {
        // Endpoints of new edges (not present in source_route) are inserted
        // into ls_checklist and later used to guide local search
        vector<uint32_t> ls_checklist;
        ls_checklist.reserve(dimension);

        for (int32_t iteration = 0 ; iteration < iterations ; ++iteration) {
            #pragma omp barrier

            // Load pheromone * heuristic for each edge connecting nearest
            // neighbors (up to cl_size)
            #pragma omp for schedule(static)
            for (uint32_t node = 0 ; node < dimension ; ++node) {
                auto cache_it = nn_product_cache.begin() + node * cl_size;
                auto heuristic_it = cl_heuristic_cache.begin() + node * cl_size;
                for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
                    *cache_it++ = *heuristic_it++ * pheromone.get(node, nn);
                }
            }

            Route local_source{ source_solution->route_, problem.get_distance_fn() };
            local_source.cost_ = source_solution->cost_;

            //Mask visited(dimension);
            Bitmask visited(dimension);

            // Changing schedule from "static" to "dynamic" can speed up
            // computations a bit, however it introduces non-determinism due to
            // threads scheduling. With "static" the computations always follow
            // the same path -- i.e. if we run the program with the same PRNG
            // seed (--seed X) then we get exactly the same results.
            #pragma omp for schedule(static, 1) reduction(+ : ant_sol_updates, local_source_sol_updates, total_new_edges, relocation_time, construction_time, average_visited_count)
            for (uint32_t ant_idx = 0; ant_idx < ants.size(); ++ant_idx) {
                const auto target_new_edges = opt.min_new_edges_;

                auto &ant = ants[ant_idx];
                // ant.initialize(dimension);
                Route route { local_source };  // We use "external" route and only copy it back to ant

                auto start_node = get_rng().next_uint32(dimension);
                // ant.visit(start_node);
                visited.clear();
                visited.set_bit(start_node);

                ls_checklist.clear();

                // We are counting edges (undirected) that are not present in
                // the source_route. The factual # of new edges can be +1 as we
                // skip the check for the closing edge (minor optimization).
                uint32_t new_edges = 0;
                auto curr_node = start_node;
                uint32_t visited_count = 1;

                double start_cs = omp_get_wtime();
                while (new_edges < target_new_edges && visited_count < dimension) {
                    auto curr = curr_node;
                    auto sel = select_next_node(pheromone, heuristic,
                                                problem.get_nearest_neighbors(curr, cl_size),
                                                nn_product_cache,
                                                problem.get_backup_neighbors(curr, cl_size, bl_size),
                                                curr,
                                                visited);

                    const auto sel_pred = route.get_pred(sel);

                    // ant.visit(sel);
                    visited.set_bit(sel);
                    ++visited_count;

                    auto start_rel = omp_get_wtime();
                    route.relocate_node(curr, sel);  // Place sel node just after curr node
                    relocation_time += omp_get_wtime() - start_rel;

                    assert(route.get_succ(curr) == sel);  // We should have (curr, sel) edge

                    if (!local_source.contains_edge(curr, sel)) {
                        /*
                        For simplicity and efficiency, we are looking only at
                        the (curr, sel) edge even though the relocation could
                        have created additional new edges.

                        Actually, we can have up to 3 new edges, however the
                        subsequent moves can break some of these, so the final
                        number can be much smaller.
                        */
                        new_edges += 1;

                        if (!contains(ls_checklist, curr)) { ls_checklist.push_back(curr); }
                        if (!contains(ls_checklist, sel)) { ls_checklist.push_back(sel); }
                        if (!contains(ls_checklist, sel_pred)) { ls_checklist.push_back(sel_pred); }
                    }
                    curr_node = sel;
                }

                average_visited_count += visited_count;
                construction_time += omp_get_wtime() - start_cs;
                if (opt.count_new_edges_) {  // How many new edges are in the new sol. actually?
                    total_new_edges += count_diff_edges(route, local_source);
                }

                if (use_ls) {
                    route.two_opt_nn(problem, ls_checklist, opt.ls_cand_list_size_);
                }

                // No need to recalculate route length -- we are updating it along with the changes
                // resp. to the current local source solution
                // ant.cost_ = problem.calculate_route_length(route.route_);
                assert( abs(problem.calculate_route_length(route.route_) - route.cost_) < 1e-6 );

                // This is a minor optimization -- if we have not found a better sol., then
                // we are unlikely to become new source solution (in the next iteration).
                // In other words, we save the new solution only if it is an improvement.
                if (!opt.keep_better_ant_sol_ 
                        || (opt.keep_better_ant_sol_ && route.cost_ < ant.cost_)) {
                    ant.cost_  = route.cost_;
                    ant.route_ = route.route_;

                    ++ant_sol_updates;
                }

                // We can benefit immediately from the improved solution by
                // updating the current local source solution.
                if (opt.source_sol_local_update_ && route.cost_ < local_source.cost_) {
                    local_source = Route{ route.route_, problem.get_distance_fn() };
                    local_source.cost_ = route.cost_;

                    ++local_source_sol_updates;
                }
                sol_costs[ant_idx] = ant.cost_;
            }

            #pragma omp master
            {
                iteration_best = &ants.front();
                for (auto &ant : ants) {
                    if (ant.cost_ < iteration_best->cost_) {
                        iteration_best = &ant;
                    }
                }
                if (iteration_best->cost_ < best_ant->cost_) {
                    best_ant->update(iteration_best->route_, iteration_best->cost_);

                    auto error = problem.calc_relative_error(best_ant->cost_);
                    best_cost_trace.add({ best_ant->cost_, error }, iteration, main_timer());

                    model.update_trail_limits(best_ant->cost_);
                }

                mean_cost_trace.add(round(sample_mean(sol_costs), 1), iteration);
                stdev_cost_trace.add(round(sample_stdev(sol_costs), 1), iteration);
            }

            // Synchronize threads before pheromone update
            #pragma omp barrier

            model.evaporate_pheromone();

            #pragma omp master
            {
                bool use_best_ant = (get_rng().next_float() < opt.gbest_as_source_prob_);
                auto &update_ant = use_best_ant ? *best_ant : *iteration_best;

                double start = omp_get_wtime();

                // Increase pheromone values on the edges of the new
                // source_solution
                model.deposit_pheromone(update_ant);

                pher_deposition_time += omp_get_wtime() - start;

                source_solution->update(update_ant.route_, update_ant.cost_);
            }
        }
    }
    comp_log("pher_deposition_time", pher_deposition_time);
    comp_log("ants solutions updates", ant_sol_updates);
    comp_log("local source solutions updates", local_source_sol_updates);
    comp_log("total new edges", total_new_edges);
    comp_log("construction time", construction_time);
    comp_log("relocation time", relocation_time);
    comp_log("average visited count", average_visited_count / iterations / ants_count);

    return unique_ptr<Solution>(dynamic_cast<Solution*>(best_ant.release()));
}
#include <vector>
#include <random>
#include <cmath>

#include <vector>
#include <algorithm>
#include <iostream>

class DynamicAntSelector {
private:
    int minAnts;          // Minimum number of ants at the start
    int maxAnts;          // Maximum number of ants at the end
    int currentAnts;      // Current ant count
    int iteration;        // Current iteration number
    int maxIterations;    // Total max iterations
    double alpha;         // Sensitivity factor for convergence rate
    int windowSize;       // How many iterations to look back for convergence

    std::vector<double> bestSolutions;  // Stores best solution values

public:
    DynamicAntSelector(int minAnts, int maxAnts, int maxIterations, double alpha = 0.1, int windowSize = 5)
        : minAnts(minAnts), maxAnts(maxAnts), currentAnts(minAnts),
          iteration(0), maxIterations(maxIterations), alpha(alpha), windowSize(windowSize) {}

    void update(double bestSolution) {
        iteration++;
        bestSolutions.push_back(bestSolution);
        
        // Maintain only the last 'windowSize' solutions
        if (bestSolutions.size() > windowSize) {
            bestSolutions.erase(bestSolutions.begin());
        }

        // Compute convergence rate if we have enough data
        if (bestSolutions.size() == windowSize) {
            double oldSolution = bestSolutions.front();
            double newSolution = bestSolutions.back();

            if (oldSolution > 0) {
                double convergenceRate = (oldSolution - newSolution) / oldSolution;
                
                // Increase ant count based on convergence rate
                if (convergenceRate < 0.01) { // If improvement is slow
                    int increase = static_cast<int>(currentAnts * (1 + alpha * convergenceRate));
                    currentAnts = std::min(maxAnts, std::max(minAnts, increase));
                }
            }
        }

        // Ensure ants increase linearly if no convergence
        int linearIncrease = minAnts + (iteration * (maxAnts - minAnts)) / maxIterations;
        currentAnts = std::max(currentAnts, linearIncrease);
    }

    int getAntCount() const {
        return currentAnts;
    }
};

template<typename ComputationsLog_t>
std::unique_ptr<Solution> 
run_flaco(const ProblemInstance &problem,
                const ProgramOptions &opt,
                ComputationsLog_t &comp_log) {

    const auto dimension  = problem.dimension_;  
    const auto cl_size    = opt.cand_list_size_;
    const auto bl_size    = opt.backup_list_size_;
    const auto ants_count = opt.ants_count_;
    const auto iterations = opt.iterations_;
    const auto use_ls     = opt.local_search_ != 0;

    const auto min_ants   = opt.min_ants_;
    const auto max_ants   = opt.max_ants_;
    const auto time_limit = opt.time_limit_;
    const auto pop_size = opt.pop_size_;
    const auto kids_size = opt.kids_size_;

    DynamicAntSelector ant_count_selector(min_ants, max_ants, iterations);

    Timer start_sol_timer;

    const auto start_routes = par_build_initial_routes(problem, use_ls);
    auto start_sol_count = start_routes.size();
    std::vector<double> start_costs(start_sol_count);

    #pragma omp parallel default(none) shared(start_sol_count, problem, start_costs, start_routes)
    #pragma omp for
    for (size_t i = 0; i < start_sol_count; ++i) {
        start_costs[i] = problem.calculate_route_length(start_routes[i]);
    }
    comp_log("initial solutions build time", start_sol_timer.get_elapsed_seconds());

    auto smallest_pos = std::distance(begin(start_costs),
                                      min_element(begin(start_costs), end(start_costs)));
    auto initial_cost = start_costs[smallest_pos];
    const auto &start_route = start_routes[smallest_pos];

    // const auto &start_route = par_build_route_ga_eax(problem, init_pop_size, init_kids_size);
    // auto initial_cost = problem.calculate_route_length(start_route);

    comp_log("initial sol cost", initial_cost);
    comp_log("initial sol error", problem.calc_relative_error(initial_cost));

    HeuristicData heuristic(problem, opt.beta_);
    vector<double> cl_heuristic_cache;

    cl_heuristic_cache.resize(cl_size * dimension);
    for (uint32_t node = 0 ; node < dimension ; ++node) {
        auto cache_it = cl_heuristic_cache.begin() + node * cl_size;

        for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
            *cache_it++ = heuristic.get(node, nn);
        }
    }

    // Probabilistic model based on pheromone trails:
    CandListModel model(problem, opt);
    // If the LS is on, the differences between pheromone trails should be
    // smaller -- we use calc_trail_limits_cl instead of calc_trail_limits
    model.calc_trail_limits_ = !use_ls ? calc_trail_limits : calc_trail_limits_cl;
    model.init(initial_cost);

    if (opt.smooth_) {
        if (opt.three_level_) {
            model.init_trail_limits_3las();
        } else {
            model.init_trail_limits_smooth();
        }
    }

    auto &pheromone = model.get_pheromone();
    pheromone.set_all_trails(model.trail_limits_.max_);

    vector<double> nn_product_cache(dimension * cl_size);

    auto best_ant = make_unique<Ant>(start_route, initial_cost);

    vector<Ant> ants(max_ants);
    for (auto &ant : ants) {
        ant = *best_ant;
    }

    Ant *iteration_best = nullptr;

    auto source_solution = make_unique<Solution>(start_route, best_ant->cost_);

    // The following are mainly for raporting purposes
    Trace<ComputationsLog_t, SolutionCost> best_cost_trace(comp_log, "best sol cost", iterations, 1, true, 1.);
    Trace<ComputationsLog_t, double> mean_cost_trace(comp_log, "sol cost mean", iterations, 20);
    Trace<ComputationsLog_t, double> stdev_cost_trace(comp_log, "sol cost stdev", iterations, 20);

    Trace<ComputationsLog_t, uint32_t> current_ant_count(comp_log, "current ant count", iterations, 1, true, 1.);

    Trace<ComputationsLog_t, uint32_t> best_ls_count(comp_log, "best ls count", iterations, 1);
    Trace<ComputationsLog_t, uint32_t> best_visited_count(comp_log, "best path count", iterations, 1);
    Trace<ComputationsLog_t, double> ls_count_average(comp_log, "ls count average", iterations, 1);
    Trace<ComputationsLog_t, double> visited_count_average(comp_log, "path count average", iterations, 1);
    Timer main_timer;

    vector<double> sol_costs(max_ants);

    double pher_deposition_time = 0;
    double construction_time = 0;
    double relocation_time = 0;
    double average_visited_count = 0;
    double current_visited_count = 0;
    double ga_time = 0;
    int32_t ant_sol_updates = 0;
    int32_t local_source_sol_updates = 0;
    int32_t total_new_edges = 0;
    uint32_t total_ls_size = 0;
    uint32_t need_phremone_update = 0;
    uint32_t actual_iterations = 0;

    InitURandom(); 

    #pragma omp parallel default(shared)
    {
        // Endpoints of new edges (not present in source_route) are inserted
        // into ls_checklist and later used to guide local search
        vector<uint32_t> ls_checklist, index(dimension);
        ls_checklist.reserve(dimension);

        std::vector<std::tuple<uint32_t, uint32_t, int64_t>> fixed_edges;
        fixed_edges.reserve(dimension);

        std::vector<Vec2d> coords;
        coords.reserve(dimension);

        std::vector<std::pair<uint32_t, uint32_t>> fixed_edges_indices;
        fixed_edges_indices.reserve(dimension);

        TEnvironment* gEnv = new TEnvironment(); 
        gEnv->fNumOfPop = pop_size;
        gEnv->fNumOfKids = kids_size;
        gEnv->fFileNameInitPop = nullptr;
        gEnv->fEvaluator->SetInstance(5000);
        gEnv->Define();

        vector<pair<uint32_t, uint32_t>> diff_edges;
        diff_edges.reserve(dimension);

        for (int32_t iteration = 0 ; iteration < iterations ; ++iteration) {
            #pragma omp barrier

            // Load pheromone * heuristic for each edge connecting nearest
            // neighbors (up to cl_size)
            #pragma omp for schedule(static)
            for (uint32_t node = 0 ; node < dimension ; ++node) {
                auto cache_it = nn_product_cache.begin() + node * cl_size;
                auto heuristic_it = cl_heuristic_cache.begin() + node * cl_size;
                for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
                    *cache_it++ = *heuristic_it++ * pheromone.get(node, nn);
                }
            }

            Route local_source{ source_solution->route_, problem.get_distance_fn() };
            local_source.cost_ = source_solution->cost_;

            //Mask visited(dimension);
            Bitmask visited(dimension);

            // ants_count = ant_count_selector.getAntCount();
            
            current_visited_count = 0;

            total_ls_size = 0;

            // Changing schedule from "static" to "dynamic" can speed up
            // computations a bit, however it introduces non-determinism due to
            // threads scheduling. With "static" the computations always follow
            // the same path -- i.e. if we run the program with the same PRNG
            // seed (--seed X) then we get exactly the same results.
            #pragma omp for schedule(static, 1) reduction(+ : ant_sol_updates, local_source_sol_updates, total_new_edges, relocation_time, construction_time, current_visited_count, total_ls_size, ga_time)
            for (uint32_t ant_idx = 0; ant_idx < ants_count; ++ant_idx) {
                const auto target_new_edges = opt.min_new_edges_;

                auto &ant = ants[ant_idx];
                // ant.initialize(dimension);
                Route route { local_source };  // We use "external" route and only copy it back to ant
                // LinkedRoute lroute { local_source };

                auto start_node = get_rng().next_uint32(dimension);

                ls_checklist.clear();

                // // insert 100 closest node to ls_checklist
                // for (auto &nn : problem.get_nearest_neighbors(start_node, opt.backup_list_size_)) {
                //     ls_checklist.push_back(nn);
                // }

                visited.clear();
                visited.set_bit(start_node);

                // We are counting edges (undirected) that are not present in
                // the source_route. The factual # of new edges can be +1 as we
                // skip the check for the closing edge (minor optimization).
                uint32_t new_edges = 0;
                auto curr_node = start_node;
                uint32_t visited_count = 1;

                double start_cs = omp_get_wtime();
                while (new_edges < target_new_edges && visited_count < dimension) {
                    auto curr = curr_node;
                    auto sel = select_next_node(pheromone, heuristic,
                                                problem.get_nearest_neighbors(curr, cl_size),
                                                nn_product_cache,
                                                problem.get_backup_neighbors(curr, cl_size, bl_size),
                                                curr,
                                                visited);

                    const auto sel_pred = route.get_pred(sel);

                    visited.set_bit(sel);
                    ++visited_count;

                    auto start_rel = omp_get_wtime();
                    route.relocate_node(curr, sel);  // Place sel node just after curr node
                    relocation_time += omp_get_wtime() - start_rel;

                    // assert(route.get_succ(curr) == sel);  // We should have (curr, sel) edge

                    if (!local_source.contains_edge(curr, sel)) {
                        /*
                        For simplicity and efficiency, we are looking only at
                        the (curr, sel) edge even though the relocation could
                        have created additional new edges.

                        Actually, we can have up to 3 new edges, however the
                        subsequent moves can break some of these, so the final
                        number can be much smaller.
                        */
                        new_edges += 1;

                        if (!contains(ls_checklist, curr)) { ls_checklist.push_back(curr); }
                        if (!contains(ls_checklist, sel)) { ls_checklist.push_back(sel); }
                        if (!contains(ls_checklist, sel_pred)) { ls_checklist.push_back(sel_pred); }
                    }
                    curr_node = sel;
                }
                
                current_visited_count += visited_count;
                construction_time += omp_get_wtime() - start_cs;

                if (opt.count_new_edges_) {  // How many new edges are in the new sol. actually?
                    total_new_edges += count_diff_edges(route, local_source);
                }

                if (use_ls) {
                    route.two_opt_nn(problem, ls_checklist, opt.ls_cand_list_size_);
                    total_ls_size += ls_checklist.size();
                    
                    // Use GA-EAX to find local optima
                }

                if (opt.use_ga_) {
                    auto start_gaeax = omp_get_wtime();
                    route.ga_eax(problem, ls_checklist, gEnv, visited, index, fixed_edges, coords, fixed_edges_indices);
                    ga_time += omp_get_wtime() - start_gaeax;
                }

                // if (opt.use_ga_) {
                //     auto start_gaeax = omp_get_wtime();
                //     diff_edges.clear();
                //     produce_diff_edges(route, local_source, diff_edges);
                //     produce_diff_edges(local_source, route, diff_edges);
                //     ls_checklist.clear();
                //     for (auto& [a, b] : diff_edges) {
                //         ls_checklist.emplace_back(a);
                //         ls_checklist.emplace_back(b);
                //     }
                //     route.ga_eax(problem, ls_checklist, gEnv, visited, index, fixed_edges, coords, fixed_edges_indices);
                //     if (route.cost_ < local_source.cost_) {
                //         cout << "GA-EAX: " << route.cost_ << " " << local_source.cost_ << endl;
                //     }
                //     ga_time += omp_get_wtime() - start_gaeax;
                // }

                // No need to recalculate route length -- we are updating it along with the changes
                // resp. to the current local source solution
                // ant.cost_ = problem.calculate_route_length(route.route_);
                assert( abs(problem.calculate_route_length(route.route_) - route.cost_) < 1e-6 );

                // This is a minor optimization -- if we have not found a better sol., then
                // we are unlikely to become new source solution (in the next iteration).
                // In other words, we save the new solution only if it is an improvement.
                if (!opt.keep_better_ant_sol_ 
                        || (opt.keep_better_ant_sol_ && route.cost_ < ant.cost_)) {
                    ant.cost_  = route.cost_;
                    ant.route_ = route.route_;
                    ant.ls_count_ = ls_checklist.size();
                    ant.path_length_ = visited_count;

                    ++ant_sol_updates;
                }

                // We can benefit immediately from the improved solution by
                // updating the current local source solution.
                if (opt.source_sol_local_update_ && route.cost_ < local_source.cost_) {
                    local_source = Route{ route.route_, problem.get_distance_fn() };
                    local_source.cost_ = route.cost_;

                    ++local_source_sol_updates;
                }
                sol_costs[ant_idx] = ant.cost_;
            }

            #pragma omp master
            {
                average_visited_count += current_visited_count / ants_count;

                iteration_best = &ants.front();
                for (auto &ant : ants) {
                    if (ant.cost_ < iteration_best->cost_) {
                        iteration_best = &ant;
                    }
                }

                // if (opt.use_ga_) {
                //     auto start_gaeax = omp_get_wtime();
                //     diff_edges.clear();
                //     produce_diff_edges(*iteration_best, *best_ant, diff_edges);
                //     ls_checklist.clear();
                //     for (auto& [a, b] : diff_edges) {
                //         ls_checklist.emplace_back(a);
                //         ls_checklist.emplace_back(b);
                //     }
                //     local_source = Route{ iteration_best->route_, problem.get_distance_fn() };
                //     local_source.cost_ = iteration_best->cost_;
                //     local_source.ga_eax(problem, ls_checklist, gEnv, visited, index, fixed_edges, coords, fixed_edges_indices);

                //     // cout << diff_edges.size() << " " << ls_checklist.size() << " " << (int64_t)iteration_best->cost_ << " " << (int64_t)local_source.cost_ << endl;

                //     iteration_best->route_ = local_source.route_;
                //     iteration_best->cost_ = local_source.cost_;
                //     ga_time += omp_get_wtime() - start_gaeax;
                // }

                if (iteration_best->cost_ < best_ant->cost_) {
                    // a list of differ edges from source_solution to iteration_best
                    if (opt.three_level_) {
                        diff_edges.clear();
                        produce_diff_edges(*iteration_best, *best_ant, diff_edges);
                    }

                    best_ant->update(iteration_best->route_, iteration_best->cost_);

                    auto error = problem.calc_relative_error(best_ant->cost_);
                    best_cost_trace.add({ best_ant->cost_, error }, iteration, main_timer());
                    // current_ant_count.add(ants_count, iteration, main_timer());
                    best_ls_count.add(iteration_best->ls_count_, iteration);
                    best_visited_count.add(iteration_best->path_length_, iteration);

                    if (!opt.smooth_) {
                        model.update_trail_limits(best_ant->cost_);
                    }

                    if (opt.lazy_pheromone_update_) {
                        need_phremone_update = 1;
                    }
                }

                mean_cost_trace.add(round(sample_mean(sol_costs), 1), iteration);
                stdev_cost_trace.add(round(sample_stdev(sol_costs), 1), iteration);
                ls_count_average.add(total_ls_size / ants_count, iteration);
                visited_count_average.add(current_visited_count / ants_count, iteration);

                // ant_count_selector.update(best_ant->cost_);
            }

            // Synchronize threads before pheromone update
            #pragma omp barrier

            if (!opt.lazy_pheromone_update_ || need_phremone_update) {
                if (opt.smooth_) {
                    model.evaporate_pheromone_smooth();
                } else {
                    model.evaporate_pheromone();
                }
            }

            #pragma omp master
            {
                bool use_best_ant = (get_rng().next_float() < opt.gbest_as_source_prob_);
                auto &update_ant = use_best_ant ? *best_ant : *iteration_best;

                double start = omp_get_wtime();

                // Increase pheromone values on the edges of the new
                // source_solution
                if (!opt.lazy_pheromone_update_ || need_phremone_update) {
                    if (opt.smooth_) {
                        model.deposit_pheromone_smooth(update_ant);
                        if (opt.three_level_) {
                            model.deposit_pheromone_smooth_3(diff_edges);
                        }
                    } else {
                        model.deposit_pheromone(update_ant);
                    }
                    actual_iterations++;
                }

                pher_deposition_time += omp_get_wtime() - start;

                source_solution->update(update_ant.route_, update_ant.cost_);

                if (opt.lazy_pheromone_update_) {
                    need_phremone_update = 0;
                }
            }
        }
    }
    comp_log("pher_deposition_time", pher_deposition_time);
    comp_log("ants solutions updates", ant_sol_updates);
    comp_log("local source solutions updates", local_source_sol_updates);
    comp_log("total new edges", total_new_edges);
    comp_log("construction time", construction_time);
    comp_log("relocation time", relocation_time);
    comp_log("average visited count", average_visited_count / iterations);
    comp_log("actual iterations", actual_iterations);
    comp_log("ga time", ga_time);

    return unique_ptr<Solution>(dynamic_cast<Solution*>(best_ant.release()));
}


template<typename ComputationsLog_t>
std::unique_ptr<Solution> 
run_raco(const ProblemInstance &problem,
                const ProgramOptions &opt,
                ComputationsLog_t &comp_log) {

    const auto dimension  = problem.dimension_;  
    const auto cl_size    = opt.cand_list_size_;
    const auto bl_size    = opt.backup_list_size_;
    auto r = 4 * sqrt(problem.dimension_);
    const auto ants_count = std::min(512u, static_cast<uint32_t>(lround(r / 64) * 64)); //opt.ants_count_;
    const auto iterations = 5000; //opt.iterations_;
    const auto use_ls     = opt.local_search_ != 0;

    cout << "Ants count: " << ants_count << endl;

    Timer start_sol_timer;
    const auto start_routes = par_build_initial_routes(problem, use_ls);
    auto start_sol_count = start_routes.size();
    std::vector<double> start_costs(start_sol_count);

    #pragma omp parallel default(none) shared(start_sol_count, problem, start_costs, start_routes)
    #pragma omp for
    for (size_t i = 0; i < start_sol_count; ++i) {
        start_costs[i] = problem.calculate_route_length(start_routes[i]);
    }
    comp_log("initial solutions build time", start_sol_timer.get_elapsed_seconds());

    auto smallest_pos = std::distance(begin(start_costs),
                                      min_element(begin(start_costs), end(start_costs)));
    auto initial_cost = start_costs[smallest_pos];
    const auto &start_route = start_routes[smallest_pos];
    comp_log("initial sol cost", initial_cost);

    HeuristicData heuristic(problem, opt.beta_);
    vector<double> cl_heuristic_cache;

    cl_heuristic_cache.resize(cl_size * dimension);
    for (uint32_t node = 0 ; node < dimension ; ++node) {
        auto cache_it = cl_heuristic_cache.begin() + node * cl_size;

        for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
            *cache_it++ = heuristic.get(node, nn);
        }
    }

    // Probabilistic model based on pheromone trails:
    CandListModel model(problem, opt);
    // If the LS is on, the differences between pheromone trails should be
    // smaller -- we use calc_trail_limits_cl instead of calc_trail_limits
    model.calc_trail_limits_ = !use_ls ? calc_trail_limits : calc_trail_limits_cl;
    model.init(initial_cost);

    if (opt.smooth_) {
        model.init_trail_limits_smooth();
        cout << "Using SMMAS: " << model.trail_limits_.min_ << '\n';
    }

    auto &pheromone = model.get_pheromone();
    pheromone.set_all_trails(model.trail_limits_.max_);

    vector<double> nn_product_cache(dimension * cl_size);

    auto best_ant = make_unique<Ant>(start_route, initial_cost);

    vector<Ant> ants(ants_count);
    for (auto &ant : ants) {
        ant = *best_ant;
    }

    Ant *iteration_best = nullptr;

    auto source_solution = make_unique<Solution>(start_route, best_ant->cost_);

    // The following are mainly for raporting purposes
    Trace<ComputationsLog_t, SolutionCost> best_cost_trace(comp_log, "best sol cost", iterations, 1, true, 1.);
    Timer main_timer;

    vector<double> sol_costs(ants_count);

    auto min_new_edges = opt.min_new_edges_;

    #pragma omp parallel default(shared)
    {
        // Endpoints of new edges (not present in source_route) are inserted
        // into ls_checklist and later used to guide local search
        vector<uint32_t> ls_checklist;
        ls_checklist.reserve(dimension);

        for (int32_t iteration = 0 ; iteration < iterations ; ++iteration) {
            #pragma omp barrier

            // Load pheromone * heuristic for each edge connecting nearest
            // neighbors (up to cl_size)
            #pragma omp for schedule(static)
            for (uint32_t node = 0 ; node < dimension ; ++node) {
                auto cache_it = nn_product_cache.begin() + node * cl_size;
                auto heuristic_it = cl_heuristic_cache.begin() + node * cl_size;
                for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
                    *cache_it++ = *heuristic_it++ * pheromone.get(node, nn);
                }
            }

            Route local_source{ source_solution->route_, problem.get_distance_fn() };
            local_source.cost_ = source_solution->cost_;

            Bitmask visited(dimension);

            // Changing schedule from "static" to "dynamic" can speed up
            // computations a bit, however it introduces non-determinism due to
            // threads scheduling. With "static" the computations always follow
            // the same path -- i.e. if we run the program with the same PRNG
            // seed (--seed X) then we get exactly the same results.
            #pragma omp for schedule(static, 1) 
            for (uint32_t ant_idx = 0; ant_idx < ants.size(); ++ant_idx) {
                const auto target_new_edges = min_new_edges;

                auto &ant = ants[ant_idx];
                Route route { local_source };  // We use "external" route and only copy it back to ant

                auto start_node = get_rng().next_uint32(dimension);
                visited.clear();
                visited.set_bit(start_node);

                ls_checklist.clear();

                // We are counting edges (undirected) that are not present in
                // the source_route. The factual # of new edges can be +1 as we
                // skip the check for the closing edge (minor optimization).
                uint32_t new_edges = 0;
                auto curr_node = start_node;
                uint32_t visited_count = 1;

                double start_cs = omp_get_wtime();
                while (new_edges < target_new_edges && visited_count < dimension) {
                    auto curr = curr_node;
                    if (opt.force_new_edges_) {
                        visited.set_bit(route.get_succ(curr));
                    }

                    auto sel = select_next_node(pheromone, heuristic,
                                                problem.get_nearest_neighbors(curr, cl_size),
                                                nn_product_cache,
                                                problem.get_backup_neighbors(curr, cl_size, bl_size),
                                                curr,
                                                visited);

                    if (opt.force_new_edges_) {
                        visited.clear_bit(route.get_succ(curr));
                    }

                    const auto sel_pred = route.get_pred(sel);

                    visited.set_bit(sel);
                    ++visited_count;

                    route.relocate_node(curr, sel); 
                    curr_node = sel;

                    if (!local_source.contains_edge(curr, sel)) {
                        new_edges += 1;

                        if (!contains(ls_checklist, curr)) { ls_checklist.push_back(curr); }
                        if (!contains(ls_checklist, sel)) { ls_checklist.push_back(sel); }
                        if (!contains(ls_checklist, sel_pred)) { ls_checklist.push_back(sel_pred); }
                    }
                }

                if (use_ls) {
                    route.two_opt_nn(problem, ls_checklist, opt.ls_cand_list_size_);
                }

                // This is a minor optimization -- if we have not found a better sol., then
                // we are unlikely to become new source solution (in the next iteration).
                // In other words, we save the new solution only if it is an improvement.
                if (!opt.keep_better_ant_sol_ 
                        || (opt.keep_better_ant_sol_ && route.cost_ < ant.cost_)) {
                    ant.cost_  = route.cost_;
                    ant.route_ = route.route_;

                }

                // We can benefit immediately from the improved solution by
                // updating the current local source solution.
                if (opt.source_sol_local_update_ && route.cost_ < local_source.cost_) {
                    local_source = Route{ route.route_, problem.get_distance_fn() };
                    local_source.cost_ = route.cost_;

                }
                sol_costs[ant_idx] = ant.cost_;
            }

            #pragma omp master
            {
                iteration_best = &ants.front();
                for (auto &ant : ants) {
                    if (ant.cost_ < iteration_best->cost_) {
                        iteration_best = &ant;
                    }
                }
                if (iteration_best->cost_ < best_ant->cost_) {
                    best_ant->update(iteration_best->route_, iteration_best->cost_);

                    auto error = problem.calc_relative_error(best_ant->cost_);
                    best_cost_trace.add({ best_ant->cost_, error }, iteration, main_timer());

                    if (!opt.smooth_) {
                        model.update_trail_limits(best_ant->cost_);
                    }
                }
            }

            // Synchronize threads before pheromone update
            #pragma omp barrier

            if (opt.smooth_) {
                model.evaporate_pheromone_smooth();
            } else {
                model.evaporate_pheromone();
            }

            #pragma omp master
            {
                bool use_best_ant = (get_rng().next_float() < opt.gbest_as_source_prob_);
                auto &update_ant = use_best_ant ? *best_ant : *iteration_best;
                // Increase pheromone values on the edges of the new
                // source_solution
                if (opt.smooth_) {
                    model.deposit_pheromone_smooth(update_ant);
                } else {
                    model.deposit_pheromone(update_ant);
                }

                source_solution->update(update_ant.route_, update_ant.cost_);

            }
        }
    }

    return unique_ptr<Solution>(dynamic_cast<Solution*>(best_ant.release()));
}


template<typename ComputationsLog_t>
std::unique_ptr<Solution> 
run_meta_flaco(const ProblemInstance &problem,
                const ProgramOptions &opt,
                ComputationsLog_t &comp_log) {

    const auto dimension  = problem.dimension_;  
    const auto cl_size    = opt.cand_list_size_;
    const auto bl_size    = opt.backup_list_size_;
    const auto ants_count = opt.ants_count_;
    const auto iterations = opt.iterations_;
    const auto use_ls     = opt.local_search_ != 0;

    const auto pop_size = opt.pop_size_;
    const auto kids_size = opt.kids_size_;


    Timer start_sol_timer;

    // const auto start_routes = par_build_initial_routes(problem, use_ls);
    // auto start_sol_count = start_routes.size();
    // std::vector<double> start_costs(start_sol_count);

    // #pragma omp parallel default(none) shared(start_sol_count, problem, start_costs, start_routes)
    // #pragma omp for
    // for (size_t i = 0; i < start_sol_count; ++i) {
    //     start_costs[i] = problem.calculate_route_length(start_routes[i]);
    // }

    // auto smallest_pos = std::distance(begin(start_costs),
    //                                   min_element(begin(start_costs), end(start_costs)));
    // auto initial_cost = start_costs[smallest_pos];
    // const auto &start_route = start_routes[smallest_pos];

    // opt.iterations_ = 5000; opt.ants_count_ = 512;
    auto init_result = run_raco(problem, opt, comp_log);
    auto start_route = init_result->route_;
    auto initial_cost = init_result->cost_;

    comp_log("initial sol cost", initial_cost);
    comp_log("initial sol error", problem.calc_relative_error(initial_cost));

    auto best_ant = make_unique<Ant>(start_route, initial_cost);

    vector<Ant> ants(ants_count);
    for (auto &ant : ants) {
        ant = *best_ant;
    }

    Ant *iteration_best = nullptr;

    auto source_solution = make_unique<Solution>(start_route, best_ant->cost_);

    // Probabilistic model based on pheromone trails:
    CandListModel model(problem, opt);
    // If the LS is on, the differences between pheromone trails should be
    // smaller -- we use calc_trail_limits_cl instead of calc_trail_limits
    model.calc_trail_limits_ = calc_trail_limits_cl;
    model.init(initial_cost);
    model.init_trail_limits_meta(opt.diff_edges_pheromone_gain_);

    auto &pheromone = model.get_pheromone();
    pheromone.set_all_trails(model.trail_limits_.min_);
    pheromone.default_pheromone_value_ = model.trail_limits_.min_;

    Timer main_timer;
    vector<double> sol_costs(ants_count);
    InitURandom(); 

    int32_t ant_sol_updates = 0;
    int32_t local_source_sol_updates = 0;
    Trace<ComputationsLog_t, SolutionCost> best_cost_trace(comp_log, "best sol cost", iterations, 1, true, 1.);
    Trace<ComputationsLog_t, uint32_t> fixed_edges_trace(comp_log, "fixed edges trace", iterations, 1);

    #pragma omp parallel default(shared)
    {
        // Endpoints of new edges (not present in source_route) are inserted
        // into ls_checklist and later used to guide local search
        vector<uint32_t> ls_checklist, index(dimension);
        // ls_checklist.reserve(dimension);
        ls_checklist.resize(dimension);

        std::vector<std::tuple<uint32_t, uint32_t, int64_t>> fixed_edges;
        fixed_edges.reserve(dimension);

        std::vector<Vec2d> coords;
        coords.reserve(dimension);

        std::vector<std::pair<uint32_t, uint32_t>> fixed_edges_indices;
        fixed_edges_indices.reserve(dimension);

        TEnvironment* gEnv = new TEnvironment(); 
        gEnv->fNumOfPop = pop_size;
        gEnv->fNumOfKids = kids_size;
        gEnv->fFileNameInitPop = nullptr;
        gEnv->fEvaluator->SetInstance(2000);
        gEnv->Define();

        vector<pair<uint32_t, uint32_t>> diff_edges;
        diff_edges.reserve(dimension);

        uint32_t total_edge_fixed = 0;

        for (int32_t iteration = 0 ; iteration < iterations ; ++iteration) {
            #pragma omp barrier

            Route local_source{ source_solution->route_, problem.get_distance_fn() };
            local_source.cost_ = source_solution->cost_;
            Bitmask visited(dimension);
            total_edge_fixed = 0;

            #pragma omp for schedule(static, 1) reduction(+ : ant_sol_updates, local_source_sol_updates)
            for (uint32_t ant_idx = 0; ant_idx < ants_count; ++ant_idx) {
                auto &ant = ants[ant_idx];
 
                Route route { local_source };  
                auto start_node = get_rng().next_uint32(dimension);

                // ls_checklist[start_node] = 1;
                // for (auto &nn : problem.get_nearest_neighbors(start_node, opt.count_delete_edges_ * 2)) {
                //     bool edge_fixed = 0; // get_rng().next_float() <= pheromone.get(nn, route.get_succ(nn)) ;
                //     if (!edge_fixed) {
                //         ls_checklist[nn] = 1;

                //         if (total_deleted >= opt.count_delete_edges_) {
                //             break;
                //         }
                //     } 
                // }

                ls_checklist[start_node] = 1;
                for (auto &nn : problem.get_nearest_neighbors(start_node, opt.count_delete_edges_)) {
                    ls_checklist[nn] = 1;
                }

                route.ga_eax_fixed_edges(problem, ls_checklist, gEnv, visited, index, fixed_edges, coords, fixed_edges_indices);

                ls_checklist[start_node] = 0;
                for (auto &nn : problem.get_nearest_neighbors(start_node, opt.count_delete_edges_)) {
                    ls_checklist[nn] = 0;
                }

                // diff_edges.clear();
                // ls_checklist[start_node] = 0;
                // for (auto &nn : problem.get_nearest_neighbors(start_node, opt.ls_cand_list_size_ * 2)) if (ls_checklist[nn]) {
                //     // diff_edges.emplace_back(nn, route.get_succ(nn));
                //     ls_checklist[nn] = 0;
                // }


                // This is a minor optimization -- if we have not found a better sol., then
                // we are unlikely to become new source solution (in the next iteration).
                // In other words, we save the new solution only if it is an improvement.
                if (!opt.keep_better_ant_sol_ 
                        || (opt.keep_better_ant_sol_ && route.cost_ < ant.cost_)) {
                    ant.cost_  = route.cost_;
                    ant.route_ = route.route_;
                    ant.ls_count_ = ls_checklist.size();
                    ant.path_length_ = opt.backup_list_size_;

                    ++ant_sol_updates;
                }

                // We can benefit immediately from the improved solution by
                // updating the current local source solution.
                if (opt.source_sol_local_update_ && route.cost_ < local_source.cost_) {
                    local_source = Route{ route.route_, problem.get_distance_fn() };
                    local_source.cost_ = route.cost_;

                    ++local_source_sol_updates;
                }
                sol_costs[ant_idx] = ant.cost_;
            }

            // Synchronize threads before pheromone update
            #pragma omp barrier
            // model.evaporate_pheromone_smooth();

            #pragma omp master
            {
                iteration_best = &ants.front();
                for (auto &ant : ants) {
                    if (ant.cost_ < iteration_best->cost_) {
                        iteration_best = &ant;
                    }
                }

                // diff_edges.clear();
                // produce_diff_edges(*iteration_best, *best_ant, diff_edges);

                if (iteration_best->cost_ < best_ant->cost_) {

                    best_ant->update(iteration_best->route_, iteration_best->cost_);

                    auto error = problem.calc_relative_error(best_ant->cost_);
                    best_cost_trace.add({ best_ant->cost_, error }, iteration, main_timer());

                    source_solution->update(best_ant->route_, best_ant->cost_);

                    // model.update_trail_limits(best_ant->cost_);
                }

                // model.deposit_pheromone_smooth(diff_edges);

                
            }
        }

        delete gEnv;
        gEnv = nullptr;
    }

    comp_log("ants solutions updates", ant_sol_updates);
    comp_log("local source solutions updates", local_source_sol_updates);

    return unique_ptr<Solution>(dynamic_cast<Solution*>(best_ant.release()));
}




std::string get_results_filename(const ProblemInstance &problem,
                                 const std::string &alg_name) {
    using namespace std;
    ostringstream out;
    out << alg_name << '-'
        << problem.name_ << '_'
        << get_current_datetime_string("-", "_", "--")
        << ".json";
    return out.str();
}

std::string get_exp_id(const std::string &id) {
    auto pos = id.find('.');
    if (pos != string::npos) {
        return id.substr(0, pos);
    }
    return id;
}

fs::path get_results_dir_path(const ProgramOptions &args) {
    fs::path res_path(args.results_dir_);
    res_path = (args.id_ != "default") ? res_path / get_exp_id(args.id_) : res_path;
    fs::create_directories(res_path);
    return res_path;
}

fs::path get_results_file_path(const ProgramOptions &args, const ProblemInstance &problem) {
    return get_results_dir_path(args) / get_results_filename(problem, args.algorithm_);
}

struct Result {
    double mean, mean_err, min_cost, min_err, max_cost, max_err, stdev, mean_time;
};

int main(int argc, char *argv[]) {
    using json = nlohmann::json;
    using Log = ComputationsLog<json>;
    using aco_fn = std::unique_ptr<Solution> (*)(const ProblemInstance &, const ProgramOptions &, Log &);

    auto args = parse_program_options(argc, argv);

    if (args.seed_ == 0) {
        std::random_device rd;
        args.seed_ = rd();
    }
    init_random_number_generators(args.seed_);

    if (args.threads_ > 0) {
        cout << "Setting # threadS:" << args.threads_ << endl;
        omp_set_num_threads(args.threads_);
    }

    try {
        json experiment_record;
        Log exp_log(experiment_record, std::cout);

        auto problem = load_tsplib_instance(args.problem_path_.c_str());
        load_best_known_solutions("best-known.json");
        problem.best_known_cost_ = get_best_known_value(problem.name_, -1);

        Timer nn_lists_timer;
        auto nn_count = args.count_delete_edges_; //std::max(args.cand_list_size_ + args.backup_list_size_,args.ls_cand_list_size_);
        problem.compute_nn_lists(nn_count);
        exp_log("nn and backup lists calc time", nn_lists_timer());

        aco_fn alg = nullptr; 
        if (args.algorithm_ == "faco") {
            alg = run_focused_aco;

            if (args.ants_count_ == 0) {
                auto r = 4 * sqrt(problem.dimension_);
                args.ants_count_ = static_cast<uint32_t>(lround(r / 64) * 64);
            }
        } else if (args.algorithm_ == "mfaco") {
            alg = run_mfaco;

            if (args.ants_count_ == 0) {
                auto r = 4 * sqrt(problem.dimension_);
                args.ants_count_ = static_cast<uint32_t>(lround(r / 64) * 64);
            }
        } else if (args.algorithm_ == "mmas") {
            alg = run_mmas<CandListModel>;

            if (args.ants_count_ == 0) {
                args.ants_count_ = problem.dimension_;
            }
        } else if (args.algorithm_ == "flaco") {
            alg = run_flaco;

            if (args.ants_count_ == 0) {
                auto r = 4 * sqrt(problem.dimension_);
                args.ants_count_ = static_cast<uint32_t>(lround(r / 64) * 64);
            }
        } else if (args.algorithm_ == "meta-flaco") {
            alg = run_meta_flaco;

            if (args.ants_count_ == 0) {
                auto r = 4 * sqrt(problem.dimension_);
                args.ants_count_ = static_cast<uint32_t>(lround(r / 64) * 64);
            }
        } else {
            cerr << "Unknown algorithm: " << args.algorithm_ << endl;
            return 1;
        }

        dump(args, experiment_record["args"]);
        experiment_record["executions"] = json::array();
        vector<double> costs;
        vector<double> times;

        Timer trial_timer;
        std::string res_filepath{};

        for (int i = 0 ; i < args.repeat_ ; ++i) {
            cout << "Starting execution: " << i << "\n";
            json execution_log;
            Log exlog(execution_log, std::cout);
            exlog("started_at", get_current_datetime_string("-", ":", "T", true));

            Timer execution_timer;
            auto result = alg(problem, args, exlog);

            if ( abs( problem.calculate_route_length(result->route_) - result->cost_ ) > 1e-6) {
                cerr << "wrong route?: " << problem.calculate_route_length(result->route_) << ' ' << result->cost_ << '\n';
                abort();
            }

            const auto execution_time = execution_timer();
            exlog("execution time", execution_time);
            exlog("finished_at", get_current_datetime_string("-", ":", "T", true));
            exlog("final cost", result->cost_);
            exlog("final error", problem.calc_relative_error(result->cost_));

            experiment_record["executions"].emplace_back(execution_log);

            costs.push_back(result->cost_);
            times.push_back(execution_time);

            bool is_last_execution = (i + 1 == args.repeat_);
            if (is_last_execution) {
                exp_log("trial time", trial_timer());

                if (args.save_route_picture_) {
                    auto filename = ((!problem.name_.empty()) ? problem.name_ : "route") + ".svg";
                    auto svg_path = get_results_dir_path(args) / filename;
                    Timer t;
                    route_to_svg(problem, result->route_, svg_path);
                    cout << "Route image saved to " << filename << " in " << t() << " seconds\n";
                }
            }

            // Write the results computed so far to a file -- this prevents
            // losing data in case of an unexpected program termination
            exp_log("trial mean cost", static_cast<int64_t>(sample_mean(costs)));
            exp_log("trial mean error", problem.calc_relative_error(sample_mean(costs)));

            auto min_cost = *min_element(begin(costs), end(costs));
            exp_log("trial min cost", static_cast<int64_t>(min_cost));
            exp_log("trial min error", problem.calc_relative_error(min_cost));

            auto max_cost = *max_element(begin(costs), end(costs));
            exp_log("trial max cost", static_cast<int64_t>(max_cost));
            exp_log("trial max error", problem.calc_relative_error(max_cost));

            exp_log("trial mean time", static_cast<int64_t>(sample_mean(times)));

            if (costs.size() > 1) {
                exp_log("trial stdev cost", sample_stdev(costs));
            }

            if (res_filepath.length() == 0) {  // On first attempt set the filename
                res_filepath = get_results_file_path(args, problem);
            }
            if (ofstream out(res_filepath); out.is_open()) {
                cout << "Saving results to: " << res_filepath << endl;
                out << experiment_record.dump(1);
                out.close();
            }
        }
    } catch (const runtime_error &e) {
        cout << "An error has occurred: " << e.what() << endl;
    }
    return 0;
}