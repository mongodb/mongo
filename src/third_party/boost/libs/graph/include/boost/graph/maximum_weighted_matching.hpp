//=======================================================================
// Copyright (c) 2018 Yi Ji
// Copyright (c) 2025 Joris van Rantwijk
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
//=======================================================================

#ifndef BOOST_GRAPH_MAXIMUM_WEIGHTED_MATCHING_HPP
#define BOOST_GRAPH_MAXIMUM_WEIGHTED_MATCHING_HPP

#include <boost/assert.hpp>
#include <boost/optional.hpp>
#include <boost/property_map/property_map.hpp>
#include <boost/range/iterator_range_core.hpp>
#include <boost/graph/exception.hpp>
#include <boost/graph/graph_concepts.hpp>
#include <boost/graph/max_cardinality_matching.hpp>  // for empty_matching

#include <algorithm>
#include <deque>
#include <limits>
#include <list>
#include <stack>
#include <tuple>  // for std::tie
#include <utility>  // for std::pair, std::swap
#include <vector>

namespace boost
{

template <typename Graph, typename MateMap, typename VertexIndexMap>
typename property_traits<
    typename property_map<Graph, edge_weight_t>::type>::value_type
matching_weight_sum(const Graph& g, MateMap mate, VertexIndexMap vm)
{
    using vertex_iterator_t = typename graph_traits<Graph>::vertex_iterator;
    using vertex_descriptor_t = typename graph_traits<Graph>::vertex_descriptor;
    using edge_property_t = typename property_traits<
        typename property_map<Graph, edge_weight_t>::type>::value_type;

    edge_property_t weight_sum = 0;
    vertex_iterator_t vi, vi_end;

    for (boost::tie(vi, vi_end) = vertices(g); vi != vi_end; ++vi)
    {
        vertex_descriptor_t v = *vi;
        if (get(mate, v) != graph_traits<Graph>::null_vertex()
            && get(vm, v) < get(vm, get(mate, v)))
            weight_sum += get(edge_weight, g, edge(v, mate[v], g).first);
    }
    return weight_sum;
}

template <typename Graph, typename MateMap>
inline typename property_traits<
    typename property_map<Graph, edge_weight_t>::type>::value_type
matching_weight_sum(const Graph& g, MateMap mate)
{
    return matching_weight_sum(g, mate, get(vertex_index, g));
}

// brute-force matcher searches all possible combinations of matched edges to
// get the maximum weighted matching which can be used for testing on small
// graphs (within dozens vertices)
template <typename Graph, typename MateMap, typename VertexIndexMap>
class brute_force_matching
{
public:
    using vertex_descriptor_t = typename graph_traits<Graph>::vertex_descriptor;
    using vertex_iterator_t = typename graph_traits<Graph>::vertex_iterator;
    using vertex_vec_iter_t =
        typename std::vector<vertex_descriptor_t>::iterator;
    using edge_iterator_t = typename graph_traits<Graph>::edge_iterator;
    using vertex_to_vertex_map_t =
        boost::iterator_property_map<vertex_vec_iter_t, VertexIndexMap>;

    brute_force_matching(
        const Graph& arg_g, MateMap arg_mate, VertexIndexMap arg_vm)
    : g(&arg_g)
    , vm(arg_vm)
    , mate_vector(num_vertices(*g))
    , best_mate_vector(num_vertices(*g))
    , mate(mate_vector.begin(), vm)
    , best_mate(best_mate_vector.begin(), vm)
    {
        vertex_iterator_t vi, vi_end;
        for (boost::tie(vi, vi_end) = vertices(*g); vi != vi_end; ++vi)
            best_mate[*vi] = mate[*vi] = get(arg_mate, *vi);
    }

    template <typename PropertyMap> void find_matching(PropertyMap pm)
    {
        edge_iterator_t ei;
        boost::tie(ei, ei_end) = edges(*g);
        select_edge(ei);

        vertex_iterator_t vi, vi_end;
        for (boost::tie(vi, vi_end) = vertices(*g); vi != vi_end; ++vi)
            put(pm, *vi, best_mate[*vi]);
    }

private:
    const Graph* g;
    VertexIndexMap vm;
    std::vector<vertex_descriptor_t> mate_vector, best_mate_vector;
    vertex_to_vertex_map_t mate, best_mate;
    edge_iterator_t ei_end;

    void select_edge(edge_iterator_t ei)
    {
        if (ei == ei_end)
        {
            if (matching_weight_sum(*g, mate)
                > matching_weight_sum(*g, best_mate))
            {
                vertex_iterator_t vi, vi_end;
                for (boost::tie(vi, vi_end) = vertices(*g); vi != vi_end; ++vi)
                    best_mate[*vi] = mate[*vi];
            }
            return;
        }

        vertex_descriptor_t v, w;
        v = source(*ei, *g);
        w = target(*ei, *g);

        select_edge(++ei);

        if (mate[v] == graph_traits<Graph>::null_vertex()
            && mate[w] == graph_traits<Graph>::null_vertex())
        {
            mate[v] = w;
            mate[w] = v;
            select_edge(ei);
            mate[v] = mate[w] = graph_traits<Graph>::null_vertex();
        }
    }
};

template <typename Graph, typename MateMap, typename VertexIndexMap>
void brute_force_maximum_weighted_matching(
    const Graph& g, MateMap mate, VertexIndexMap vm)
{
    empty_matching<Graph, MateMap>::find_matching(g, mate);
    brute_force_matching<Graph, MateMap, VertexIndexMap> brute_force_matcher(
        g, mate, vm);
    brute_force_matcher.find_matching(mate);
}

template <typename Graph, typename MateMap>
inline void brute_force_maximum_weighted_matching(const Graph& g, MateMap mate)
{
    brute_force_maximum_weighted_matching(g, mate, get(vertex_index, g));
}

namespace graph
{
namespace detail
{

/** Check that vertex indices are unique and in range [0, V). */
template <typename Graph, typename VertexIndexMap>
void check_vertex_index_range(const Graph& g, VertexIndexMap vm)
{
    using index_t = typename property_traits<VertexIndexMap>::value_type;
    using unsigned_index_t = typename std::make_unsigned<index_t>::type;
    auto nv = num_vertices(g);
    std::vector<bool> got_vertex(nv);
    for (const auto& x : make_iterator_range(vertices(g)))
    {
        index_t i = get(vm, x);
        if ((i < 0) || (static_cast<unsigned_index_t>(i) >= nv))
            throw bad_graph("Invalid vertex index.");
        if (got_vertex[i])
            throw bad_graph("Duplicate vertex index.");
        got_vertex[i] = true;
    }
}

/** Check that edge weights are valid. */
template <typename Graph, typename EdgeWeightMap>
void check_maximum_weighted_matching_edge_weights(
    const Graph& g, EdgeWeightMap edge_weights)
{
    for (const auto& e : make_iterator_range(edges(g)))
    {
        auto w = get(edge_weights, e);
        auto max_weight = (std::numeric_limits<decltype(w)>::max)() / 4;
        if (!(w <= max_weight))  // inverted logic to catch NaN
            throw bad_graph("Edge weight exceeds maximum supported value.");
    }
}

/** Implementation of the matching algorithm. */
template <typename Graph, typename VertexIndexMap, typename EdgeWeightMap>
struct maximum_weighted_matching_context
{
    using vertex_t = typename graph_traits<Graph>::vertex_descriptor;
    using edge_t = typename graph_traits<Graph>::edge_descriptor;
    using vertices_size_t = typename graph_traits<Graph>::vertices_size_type;
    using edges_size_t = typename graph_traits<Graph>::edges_size_type;
    using weight_t = typename property_traits<EdgeWeightMap>::value_type;

    /** Ordered pair of vertices. */
    using vertex_pair_t = std::pair<vertex_t, vertex_t>;

    /**
     * List of edges forming an alternating path or alternating cycle.
     *
     * The path is defined over top-level blossoms; it skips parts of the path
     * that are internal to blossoms. Vertex pairs are oriented to match the
     * direction of the path.
     */
    using alternating_path_t = std::deque<vertex_pair_t>;

    /** Top-level blossoms may be labeled "S" or "T" or unlabeled. */
    enum blossom_label_t { LABEL_NONE = 0, LABEL_S = 1, LABEL_T = 2 };

    struct nontrivial_blossom_t;  // forward declaration

    /**
     * A blossom is either a single vertex, or an odd-length alternating
     * cycle over sub-blossoms.
     */
    struct blossom_t
    {
        /** Parent of this blossom, or "nullptr" for top-level blossom. */
        nontrivial_blossom_t* parent;

        /**
         * Base vertex of this blossom.
         *
         * This is the unique vertex inside the blossom which is not matched
         * to another vertex in the same blossom.
         */
        vertex_t base_vertex;

        /** Label S or T or NONE, only valid for top-level blossoms. */
        blossom_label_t label;

        /** True if this is an instance of nontrivial_blossom. */
        bool is_nontrivial_blossom;

        /** Edge that attaches this blossom to the alternating tree. */
        vertex_pair_t tree_edge;

        /** Base vertex of the blossom at the root of the alternating tree. */
        vertex_t tree_root;

        /** Least-slack edge to a different S-blossom. */
        optional<edge_t> best_edge;

        /** Initialize a blossom instance. */
        blossom_t(vertex_t base_vertex = graph_traits<Graph>::null_vertex(),
                  bool is_nontrivial_blossom = false)
          : parent(nullptr)
          , base_vertex(base_vertex)
          , label(LABEL_NONE)
          , is_nontrivial_blossom(is_nontrivial_blossom)
        { }

        /**
         * Cast this blossom_t instance to nontrivial_blossom_t if possible,
         * otherwise return "nullptr".
         */
        nontrivial_blossom_t* nontrivial()
        {
            // This is much faster than dynamic_cast.
            return (is_nontrivial_blossom ?
                static_cast<nontrivial_blossom_t*>(this) : nullptr);
        }

        const nontrivial_blossom_t* nontrivial() const
        {
            return (is_nontrivial_blossom ?
                static_cast<const nontrivial_blossom_t*>(this) : nullptr);
        }
    };

    /**
     * A non-trivial blossom is an odd-length alternating cycle over
     * 3 or more sub-blossoms.
     */
    struct nontrivial_blossom_t : public blossom_t
    {
        struct sub_blossom_t {
            /** Pointer to sub-blossom. */
            blossom_t* blossom;

            /** Vertex pair (x, y) where "x" is a vertex in "blossom" and
                "y" is a vertex in the next sub-blossom. */
            vertex_pair_t edge;
        };

        /** List of sub-blossoms, ordered along the alternating cycle. */
        std::list<sub_blossom_t> subblossoms;

        /** Dual LPP variable for this blossom. */
        weight_t dual_var;

        /** Least-slack edges to other S-blossoms. */
        std::list<edge_t> best_edge_set;

        /** Initialize a non-trivial blossom. */
        nontrivial_blossom_t(
            const std::vector<blossom_t*>& blossoms,
            const std::deque<vertex_pair_t>& edges)
          : blossom_t(blossoms.front()->base_vertex, true)
          , dual_var(0)
        {
            BOOST_ASSERT(blossoms.size() == edges.size());
            BOOST_ASSERT(blossoms.size() % 2 == 1);
            BOOST_ASSERT(blossoms.size() >= 3);

            auto blossom_it = blossoms.begin();
            auto blossom_end = blossoms.end();
            auto edge_it = edges.begin();
            while (blossom_it != blossom_end) {
                subblossoms.push_back({*blossom_it, *edge_it});
                ++blossom_it;
                ++edge_it;
            }
        }

        /** Find the position of the specified subblossom. */
        std::pair<vertices_size_t, typename std::list<sub_blossom_t>::iterator>
        find_subblossom(blossom_t* child)
        {
            vertices_size_t pos = 0;
            auto it = subblossoms.begin();
            while (it->blossom != child) {
                ++it;
                ++pos;
                BOOST_ASSERT(it != subblossoms.end());
            }
            return std::make_pair(pos, it);
        }
    };

    /** Specification of a delta step. */
    struct delta_step_t
    {
        /** Type of delta step: 1, 2, 3 or 4. */
        int kind;

        /** Delta value. */
        weight_t value;

        /** Edge on which the minimum delta occurs (for delta type 2 or 3). */
        edge_t edge;

        /** Blossom on which the minimum delta occurs (for delta type 4). */
        nontrivial_blossom_t* blossom;
    };

    /** Similar to vector_property_map, but uses a fixed-size vector. */
    template <typename T>
    struct vertex_map
    {
        using key_type = typename property_traits<VertexIndexMap>::key_type;
        std::vector<T> vec;
        VertexIndexMap vm;

        vertex_map(vertices_size_t arg_size, VertexIndexMap arg_vm)
          : vec(arg_size)
          , vm(arg_vm)
        { }

        const T& operator[](const key_type& v) const
        {
            return vec[get(vm, v)];
        }

        T& operator[](const key_type& v)
        {
            return vec[get(vm, v)];
        }
    };

    /** Keep track of the least-slack edge out of a bunch of edges. */
    struct least_slack_edge_t
    {
        optional<edge_t> edge;
        weight_t slack;

        least_slack_edge_t() : slack(0) {}

        void update(const edge_t& e, weight_t s)
        {
            if ((!edge.has_value()) || (s < slack))
            {
                edge = e;
                slack = s;
            }
        }
    };

    /** Scale integer edge weights to enable integer-only calculations. */
    static constexpr weight_t weight_factor =
        std::numeric_limits<weight_t>::is_integer ? 2 : 1;

    /** Input graph. */
    const Graph* g;
    VertexIndexMap vm;
    EdgeWeightMap edge_weights;

    /** Current mate of each vertex. */
    vertex_map<vertex_t> vertex_mate;

    /**
     * For each vertex, the trivial blossom that contains it.
     *
     * Pointers to blossoms must remain valid for the life time of
     * this data structure, therefore the underlying vector must
     * have a fixed size.
     */
    vertex_map<blossom_t> trivial_blossom;

    /**
     * List of non-trivial blossoms.
     *
     * This must be a linked list to ensure that elements can be added
     * and removed without invalidating pointers to other elements.
     */
    std::list<nontrivial_blossom_t> nontrivial_blossom;

    /** For each vertex, the unique top-level blossom that contains it. */
    vertex_map<blossom_t*> vertex_top_blossom;

    /** For each vertex, a variable in the dual LPP. */
    vertex_map<weight_t> vertex_dual;

    /** For T-vertex or unlabeled vertex, least-slack edge to any S-vertex. */
    vertex_map<optional<edge_t>> vertex_best_edge;

    /** Queue of S-vertices to be scanned. */
    std::deque<vertex_t> scan_queue;

    /** Initialize the matching algorithm. */
    explicit maximum_weighted_matching_context(
        const Graph& arg_g, VertexIndexMap arg_vm, EdgeWeightMap weights)
      : g(&arg_g)
      , vm(arg_vm)
      , edge_weights(weights)
      , vertex_mate(num_vertices(*g), arg_vm)
      , trivial_blossom(num_vertices(*g), arg_vm)
      , vertex_top_blossom(num_vertices(*g), arg_vm)
      , vertex_dual(num_vertices(*g), arg_vm)
      , vertex_best_edge(num_vertices(*g), arg_vm)
    {
        // Vertex duals are initialized to half the maximum edge weight.
        weight_t max_weight = 0;
        for (const edge_t& e : make_iterator_range(edges(*g)))
            max_weight = (std::max)(max_weight, get(weights, e));
        weight_t init_vertex_dual = max_weight * (weight_factor / 2);

        for (const vertex_t& x : make_iterator_range(vertices(*g)))
        {
            vertex_mate[x] = null_vertex();
            trivial_blossom[x].base_vertex = x;
            vertex_top_blossom[x] = &trivial_blossom[x];
            vertex_dual[x] = init_vertex_dual;
        }
    }

    /** Return the null vertex. */
    static vertex_t null_vertex()
    {
        return graph_traits<Graph>::null_vertex();
    }

    /** Call a function for every vertex inside the specified blossom. */
    template <typename Func>
    static void for_vertices_in_blossom(const blossom_t& blossom, Func func)
    {
        auto ntb = blossom.nontrivial();
        if (ntb) {
            // Visit all vertices in the non-trivial blossom.
            // Use an explicit stack to avoid deep call chains.
            std::vector<const nontrivial_blossom_t*> stack;
            stack.push_back(ntb);
            while (!stack.empty()) {
                auto cur = stack.back();
                stack.pop_back();
                for (const auto& sub : cur->subblossoms) {
                    ntb = sub.blossom->nontrivial();
                    if (ntb) {
                        stack.push_back(ntb);
                    } else {
                        func(sub.blossom->base_vertex);
                    }
                }
            }
        } else {
            // A trivial blossom contains just one vertex.
            func(blossom.base_vertex);
        }
    }

    /** Mark blossom as the top-level blossom of its vertices. */
    void update_top_level_blossom(blossom_t& blossom)
    {
        BOOST_ASSERT(!blossom.parent);
        for_vertices_in_blossom(blossom,
            [this,&blossom](vertex_t x)
            {
                vertex_top_blossom[x] = &blossom;
            });
    }

    /**
     * Calculate edge slack.
     * The two vertices must be in different top-level blossoms.
     */
    weight_t edge_slack(const edge_t& e) const
    {
        vertex_t x = source(e, *g);
        vertex_t y = target(e, *g);
        weight_t w = get(edge_weights, e);
        BOOST_ASSERT(vertex_top_blossom[x] != vertex_top_blossom[y]);
        return vertex_dual[x] + vertex_dual[y] - weight_factor * w;
    }

    /** Clear least-slack edge tracking. */
    void clear_best_edge()
    {
        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            vertex_best_edge[x].reset();
            trivial_blossom[x].best_edge.reset();
        }

        for (nontrivial_blossom_t& b : nontrivial_blossom)
        {
            b.best_edge.reset();
            b.best_edge_set.clear();
        }
    }

    /** Add edge from unlabeled verter or T-vertex "y" to an S-vertex. */
    void add_delta2_edge(vertex_t y, const edge_t& e, weight_t slack)
    {
        auto& best_edge = vertex_best_edge[y];
        if ((!best_edge.has_value()) || slack < edge_slack(*best_edge))
            best_edge = e;
    }

    /** Return least-slack edge between any unlabeled vertex and S-vertex. */
    least_slack_edge_t get_least_slack_delta2_edge()
    {
        least_slack_edge_t best_edge;
        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            if (vertex_top_blossom[x]->label == LABEL_NONE)
            {
                const auto& vertex_edge = vertex_best_edge[x];
                if (vertex_edge.has_value())
                    best_edge.update(*vertex_edge, edge_slack(*vertex_edge));
            }
        }
        return best_edge;
    }

    /** Add edge between different S-blossoms. */
    void add_delta3_edge(blossom_t& b, const edge_t& e, weight_t slack)
    {
        auto& best_edge = b.best_edge;
        if ((!best_edge.has_value()) || slack < edge_slack(*best_edge))
            best_edge = e;

        auto ntb = b.nontrivial();
        if (ntb)
            ntb->best_edge_set.push_back(e);
    }

    /** Update least-slack edge tracking after merging blossoms. */
    void merge_delta3_blossoms(nontrivial_blossom_t& blossom)
    {
        // Build a temporary array holding the least-slack edges to
        // other S-blossoms. The array is indexed by base vertex.
        std::vector<least_slack_edge_t> tmp_best_edge(num_vertices(*g));

        // Collect edges from sub-blossoms that were S-blossoms.
        for (auto& sub : blossom.subblossoms)
        {
            blossom_t* b = sub.blossom;
            if (b->label == LABEL_S)
            {
                b->best_edge.reset();
                auto ntb = b->nontrivial();
                if (ntb)
                {
                    // Use least-slack edges from subblossom.
                    for (const edge_t& e : ntb->best_edge_set)
                    {
                        blossom_t* bx = vertex_top_blossom[source(e, *g)];
                        blossom_t* by = vertex_top_blossom[target(e, *g)];
                        BOOST_ASSERT(bx == &blossom);
                        // Only use edges between top-level blossoms.
                        if (bx != by)
                        {
                            BOOST_ASSERT(by->label == LABEL_S);
                            tmp_best_edge[get(vm, by->base_vertex)].update(
                                e, edge_slack(e));
                        }
                    }
                    ntb->best_edge_set.clear();
                }
                else
                {
                    // Trivial blossoms don't maintain a least-slack edge set.
                    // Consider all incident edges.
                    for (const edge_t& e :
                        make_iterator_range(out_edges(b->base_vertex, *g)))
                    {
                        BOOST_ASSERT(source(e, *g) == b->base_vertex);
                        vertex_t y = target(e, *g);
                        blossom_t* by = vertex_top_blossom[y];
                        // Only use edges to different S-blossoms.
                        // Ignore edges with negative weight.
                        if ((by != &blossom)
                            && (by->label == LABEL_S)
                            && (get(edge_weights, e) >= 0))
                        {
                            tmp_best_edge[get(vm, by->base_vertex)].update(
                                e, edge_slack(e));
                        }
                    }
                }
            }
        }

        // Build a compact list of edges from the temporary array.
        // Also find the overall least-slack edge to any other S-blossom.
        BOOST_ASSERT(blossom.best_edge_set.empty());
        BOOST_ASSERT(!blossom.best_edge.has_value());
        least_slack_edge_t best_edge;
        for (const least_slack_edge_t& item : tmp_best_edge)
        {
            if (item.edge.has_value())
            {
                blossom.best_edge_set.push_back(*item.edge);
                best_edge.update(*item.edge, item.slack);
            }
        }
        blossom.best_edge = best_edge.edge;
    }

    /** Return least-slack edge between any pair of S-blossoms. */
    least_slack_edge_t get_least_slack_delta3_edge()
    {
        least_slack_edge_t best_edge;

        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            blossom_t* b = vertex_top_blossom[x];
            BOOST_ASSERT(!b->parent);
            if ((b->label == LABEL_S) && (b->best_edge.has_value()))
                best_edge.update(*b->best_edge, edge_slack(*b->best_edge));
        }

        return best_edge;
    }

    /** Add the vertices in a blossom to the scan queue. */
    void add_vertices_to_scan_queue(blossom_t& blossom)
    {
        for_vertices_in_blossom(blossom,
            [this](vertex_t x)
            {
                scan_queue.push_back(x);
            });
    }

    /** Trace back through the alternating trees from vertices "x" and "y". */
    alternating_path_t trace_alternating_paths(vertex_t x, vertex_t y)
    {
        // Initialize a path containing only the edge (x, y).
        alternating_path_t path;
        path.emplace_back(x, y);

        // Trace alternating path from "x" to the root of the tree.
        while (x != null_vertex())
        {
            blossom_t* bx = vertex_top_blossom[x];
            x = bx->tree_edge.first;
            if (x != null_vertex())
                path.push_front(bx->tree_edge);
        }

        // Trace alternating path from "y" to the root of the tree.
        while (y != null_vertex())
        {
            blossom_t* by = vertex_top_blossom[y];
            y = by->tree_edge.first;
            if (y != null_vertex())
                path.emplace_back(by->tree_edge.second, y);
        }

        // If we found a common ancestor, trim the paths so they end there.
        while (path.front().second == path.back().first)
        {
            BOOST_ASSERT(path.size() > 2);
            path.pop_front();
            path.pop_back();
        }

        // Any alternating path between S-blossoms must have odd length.
        BOOST_ASSERT(path.size() % 2 == 1);

        return path;
    }

    /** Create a new S-blossom from an alternating cycle. */
    void make_blossom(const alternating_path_t& path)
    {
        BOOST_ASSERT(path.size() % 2 == 1);
        BOOST_ASSERT(path.size() >= 3);

        // Collect pointers to sub-blossoms.
        std::vector<blossom_t*> subblossoms;
        subblossoms.reserve(path.size());
        for (const vertex_pair_t& edge : path)
            subblossoms.push_back(vertex_top_blossom[edge.first]);

        // Check that the path is cyclic.
        vertices_size_t pos = 0;
        for (const vertex_pair_t& edge : path)
        {
            pos = (pos + 1) % subblossoms.size();
            BOOST_ASSERT(vertex_top_blossom[edge.second] == subblossoms[pos]);
        }

        // Create the new blossom.
        nontrivial_blossom.emplace_back(subblossoms, path);
        nontrivial_blossom_t& blossom = nontrivial_blossom.back();

        // Link the subblossoms to the new parent.
        // Insert former T-vertices into the scan queue.
        for (blossom_t* b : subblossoms)
        {
            BOOST_ASSERT(!b->parent);
            b->parent = &blossom;
            if (b->label == LABEL_T)
                add_vertices_to_scan_queue(*b);
        }

        // Mark vertices as belonging to the new blossom.
        update_top_level_blossom(blossom);

        // Assign label S to the new blossom and link to the alternating tree.
        BOOST_ASSERT(subblossoms.front()->label == LABEL_S);
        blossom.label = LABEL_S;
        blossom.tree_edge = subblossoms.front()->tree_edge;
        blossom.tree_root = subblossoms.front()->tree_root;

        // Merge least-slack edges for the S-sub-blossoms.
        merge_delta3_blossoms(blossom);
    }

    /** Expand and delete a T-blossom. */
    void expand_t_blossom(nontrivial_blossom_t* blossom)
    {
        BOOST_ASSERT(!blossom->parent);
        BOOST_ASSERT(blossom->label == LABEL_T);

        // Convert sub-blossoms into top-level blossoms.
        for (const auto& sub : blossom->subblossoms)
        {
            blossom_t* b = sub.blossom;
            BOOST_ASSERT(b->parent == blossom);
            b->parent = nullptr;
            b->label = LABEL_NONE;
            update_top_level_blossom(*b);
        }

        // Reconstruct the alternating tree via the sub-blossoms.
        // Find the sub-blossom that attaches the expanding blossom to
        // the alternating tree.
        blossom_t* entry = vertex_top_blossom[blossom->tree_edge.second];

        auto subblossom_loc = blossom->find_subblossom(entry);
        auto entry_pos = subblossom_loc.first;
        auto entry_it = subblossom_loc.second;

        // Get the edge that attached this blossom to the alternating tree.
        vertex_t x, y;
        std::tie(x, y) = blossom->tree_edge;

        // Reconstruct the tree in an even number of steps from entry to base.
        auto sub_it = entry_it;
        if (entry_pos % 2 == 0)
        {
            // Walk backward around the blossom.
            auto sub_begin = blossom->subblossoms.begin();
            while (sub_it != sub_begin)
            {
                extend_tree_s_to_t(x, y);
                --sub_it;
                BOOST_ASSERT(sub_it != sub_begin);
                --sub_it;
                std::tie(y, x) = sub_it->edge;
            }
        }
        else
        {
            // Walk forward around the blossom.
            auto sub_end = blossom->subblossoms.end();
            while (sub_it != sub_end) {
                extend_tree_s_to_t(x, y);
                ++sub_it;
                BOOST_ASSERT(sub_it != sub_end);
                std::tie(x, y) = sub_it->edge;
                ++sub_it;
            }
        }

        // Assign label T to the base sub-blossom.
        blossom_t* base = blossom->subblossoms.front().blossom;
        base->label = LABEL_T;
        base->tree_edge = std::make_pair(x, y);
        base->tree_root = blossom->tree_root;

        // Delete the expanded blossom.
        auto blossom_it = std::find_if(
            nontrivial_blossom.begin(),
            nontrivial_blossom.end(),
            [blossom](const nontrivial_blossom_t& b)
            {
                return (&b == blossom);
            });
        BOOST_ASSERT(blossom_it != nontrivial_blossom.end());
        nontrivial_blossom.erase(blossom_it);
    }

    void augment_blossom_rec(
        nontrivial_blossom_t& blossom, blossom_t& entry,
        std::stack<std::pair<nontrivial_blossom_t*, blossom_t*>>& stack)
    {
        auto subblossom_loc = blossom.find_subblossom(&entry);
        auto entry_pos = subblossom_loc.first;
        auto entry_it = subblossom_loc.second;

        // Walk from entry to the base in an even number of steps.
        auto sub_begin = blossom.subblossoms.begin();
        auto sub_end = blossom.subblossoms.end();
        auto sub_it = entry_it;
        while ((sub_it != sub_begin) && (sub_it != sub_end)) {
            vertex_t x, y;
            blossom_t* bx;
            blossom_t* by;

            if (entry_pos % 2 == 0)
            {
                // Walk backward around the blossom.
                --sub_it;
                by = sub_it->blossom;
                BOOST_ASSERT(sub_it != sub_begin);
                --sub_it;
                bx = sub_it->blossom;
                std::tie(x, y) = sub_it->edge;
            }
            else
            {
                // Walk forward around the blossom.
                ++sub_it;
                BOOST_ASSERT(sub_it != sub_end);
                std::tie(x, y) = sub_it->edge;
                bx = sub_it->blossom;
                ++sub_it;
                by = (sub_it == sub_end) ?
                    blossom.subblossoms.front().blossom :
                    sub_it->blossom;
            }

            // Pull this edge into the matching.
            vertex_mate[x] = y;
            vertex_mate[y] = x;

            // Augment through any non-trivial subblossoms touching this edge.
            auto bx_ntb = bx->nontrivial();
            if (bx_ntb)
                stack.emplace(bx_ntb, &trivial_blossom[x]);

            auto by_ntb = by->nontrivial();
            if (by_ntb)
                stack.emplace(by_ntb, &trivial_blossom[y]);
        }

        // Re-orient the blossom.
        if (entry_it != sub_begin)
            blossom.subblossoms.splice(
                sub_begin, blossom.subblossoms, entry_it, sub_end);
        blossom.base_vertex = entry.base_vertex;
    }

    void augment_blossom(nontrivial_blossom_t& blossom, blossom_t& entry)
    {
        // Use an explicit stack to avoid deep call chains.
        std::stack<std::pair<nontrivial_blossom_t*, blossom_t*>> stack;
        stack.emplace(&blossom, &entry);

        while (!stack.empty()) {
            nontrivial_blossom_t* outer_blossom;
            blossom_t* inner_entry;
            std::tie(outer_blossom, inner_entry) = stack.top();

            nontrivial_blossom_t* inner_blossom = inner_entry->parent;
            BOOST_ASSERT(inner_blossom);

            if (inner_blossom != outer_blossom)
                stack.top() = std::make_pair(outer_blossom, inner_blossom);
            else
                stack.pop();

            augment_blossom_rec(*inner_blossom, *inner_entry, stack);
        }
    }

    /** Augment the matching through the specified augmenting path. */
    void augment_matching(const alternating_path_t& path)
    {
        BOOST_ASSERT(path.size() % 2 == 1);

        // Process the unmatched edges on the augmenting path.
        auto path_it = path.begin();
        auto path_end = path.end();
        while (path_it != path_end)
        {
            vertex_t x = path_it->first;
            vertex_t y = path_it->second;

            // Augment any non-trivial blossoms that touch this edge.
            blossom_t* bx = vertex_top_blossom[x];
            auto bx_ntb = bx->nontrivial();
            if (bx_ntb)
                augment_blossom(*bx_ntb, trivial_blossom[x]);

            blossom_t* by = vertex_top_blossom[y];
            auto by_ntb = by->nontrivial();
            if (by_ntb)
                augment_blossom(*by_ntb, trivial_blossom[y]);

            // Pull this edge into the matching.
            vertex_mate[x] = y;
            vertex_mate[y] = x;

            // Go to the next unmatched edge on the path.
            ++path_it;
            if (path_it == path_end)
                break;
            ++path_it;
        }
    }

    /**
     * Remove non-S-vertices from the scan queue.
     * This is necessary after removing labels from S-blossoms.
     */
    void refresh_scan_queue()
    {
        std::deque<vertex_t> new_scan_queue;
        for (const vertex_t& x : scan_queue)
        {
            if (vertex_top_blossom[x]->label == LABEL_S)
                new_scan_queue.push_back(x);
        }
        scan_queue = std::move(new_scan_queue);
    }

    /** Remove edges to non-S-vertices from delta3 edge tracking. */
    void refresh_delta3_blossom(blossom_t& b)
    {
        // Do nothing if this blossom was not tracking any delta3 edge.
        if (!b.best_edge.has_value())
            return;

        auto ntb = b.nontrivial();
        if (ntb)
        {
            // Remove bad edges from best_edge_set and refresh best_edge.
            least_slack_edge_t best_edge;
            auto it = ntb->best_edge_set.begin();
            auto it_end = ntb->best_edge_set.end();
            while (it != it_end)
            {
                vertex_t y = target(*it, *g);
                blossom_t* by = vertex_top_blossom[y];
                BOOST_ASSERT(by != &b);
                if (by->label == LABEL_S)
                {
                    best_edge.update(*it, edge_slack(*it));
                    ++it;
                }
                else
                {
                    // Remove edge to non-S-blossom.
                    it = ntb->best_edge_set.erase(it);
                }
            }
            b.best_edge = best_edge.edge;
        }
        else
        {
            // Trivial blossom does not maintain best_edge_set.
            // If its current best_edge is invalid, recompute it.
            vertex_t x = b.base_vertex;
            vertex_t y = target(*b.best_edge, *g);
            blossom_t* by = vertex_top_blossom[y];
            BOOST_ASSERT(by != &b);
            if (by->label != LABEL_S)
            {
                // Consider all incident edges to recompute best_edge.
                least_slack_edge_t best_edge;
                for (const edge_t& e : make_iterator_range(out_edges(x, *g)))
                {
                    BOOST_ASSERT(source(e, *g) == x);
                    y = target(e, *g);
                    by = vertex_top_blossom[y];
                    if ((by != &b) && (by->label == LABEL_S))
                        best_edge.update(e, edge_slack(e));
                }
                b.best_edge = best_edge.edge;
            }
        }
    }

    /** Recompute vertex_best_edge for the specified vertex. */
    void recompute_vertex_best_edge(vertex_t x)
    {
        least_slack_edge_t best_edge;

        for (const edge_t& e : make_iterator_range(out_edges(x, *g)))
        {
            BOOST_ASSERT(source(e, *g) == x);
            vertex_t y = target(e, *g);
            blossom_t* by = vertex_top_blossom[y];
            if (by->label == LABEL_S)
                best_edge.update(e, edge_slack(e));
        }
        vertex_best_edge[x] = best_edge.edge;
    }

    /** Remove the alternating trees with specified root vertices. */
    void remove_alternating_tree(vertex_t r1, vertex_t r2)
    {
        // Find blossoms that are part of the specified alternating trees.
        std::vector<blossom_t*> former_s_blossoms;
        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            blossom_t* b = vertex_top_blossom[x];
            if ((!b->parent)
                && (b->label != LABEL_NONE)
                && (b->tree_root == r1 || b->tree_root == r2))
            {
                // Build list of former S-blossoms.
                if (b->label == LABEL_S)
                    former_s_blossoms.push_back(b);

                // Remove label.
                b->label = LABEL_NONE;
            }
        }

        vertex_map<char> blossom_recompute_best_edge(num_vertices(*g), vm);
        vertex_map<char> vertex_recompute_best_edge(num_vertices(*g), vm);

        // Visit former S-blossoms.
        for (blossom_t* b : former_s_blossoms)
        {
            // Clear best_edge tracking.
            b->best_edge.reset();
            auto ntb = b->nontrivial();
            if (ntb)
                ntb->best_edge_set.clear();

            // Visit edges that touch this blossom.
            for_vertices_in_blossom(*b,
                [&](vertex_t x)
                {
                    // Mark former S-vertices.
                    vertex_recompute_best_edge[x] = 1;

                    for (const edge_t& e :
                         make_iterator_range(out_edges(x, *g)))
                    {
                        // Mark S-blossoms adjacent to the former S-blossom.
                        BOOST_ASSERT(source(e, *g) == x);
                        vertex_t y = target(e, *g);
                        blossom_t* by = vertex_top_blossom[y];
                        if (by->label == LABEL_S)
                            blossom_recompute_best_edge[by->base_vertex] = 1;

                        // Mark non-S-vertices with least-slack edge to
                        // former S-blossom.
                        if (by->label != LABEL_S)
                        {
                            const auto& best_edge = vertex_best_edge[y];
                            if (best_edge.has_value() && (*best_edge == e))
                                vertex_recompute_best_edge[y] = 1;
                        }
                    }
                });
        }

        // Recompute delta3 tracking of affected S-blossoms.
        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            if (blossom_recompute_best_edge[x])
                refresh_delta3_blossom(*vertex_top_blossom[x]);
        }

        // Recompute vertex_best_edge of affected non-S-vertices.
        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            if (vertex_recompute_best_edge[x])
                recompute_vertex_best_edge(x);
        }

        refresh_scan_queue();
    }

    /**
     * Extend the alternating tree via the edge from S-vertex "x"
     * to unlabeled vertex "y".
     *
     * Assign label T to the blossom that contains "y", then assign
     * label S to the blossom matched to the newly labeled T-blossom.
     */
    void extend_tree_s_to_t(vertex_t x, vertex_t y)
    {
        blossom_t* bx = vertex_top_blossom[x];
        blossom_t* by = vertex_top_blossom[y];

        BOOST_ASSERT(bx->label == LABEL_S);
        BOOST_ASSERT(by->label == LABEL_NONE);
        by->label = LABEL_T;
        by->tree_edge = std::make_pair(x, y);
        by->tree_root = bx->tree_root;

        vertex_t y2 = by->base_vertex;
        vertex_t z = vertex_mate[y2];
        BOOST_ASSERT(z != null_vertex());

        blossom_t* bz = vertex_top_blossom[z];
        BOOST_ASSERT(bz->label == LABEL_NONE);
        BOOST_ASSERT(!bz->best_edge.has_value());
        bz->label = LABEL_S;
        bz->tree_edge = std::make_pair(y2, z);
        bz->tree_root = by->tree_root;
        add_vertices_to_scan_queue(*bz);
    }

    /**
     * Add the edge between S-vertices "x" and "y".
     *
     * If the edge connects blossoms that are part of the same alternating
     * tree, a new S-blossom is created.
     *
     * If the edge connects two different alternating trees, an augmenting
     * path has been discovered. In this case the matching is augmented.
     *
     * @return true if the matching was augmented; otherwise false.
     */
    bool add_s_to_s_edge(vertex_t x, vertex_t y)
    {
        blossom_t* bx = vertex_top_blossom[x];
        blossom_t* by = vertex_top_blossom[y];
        BOOST_ASSERT(bx->label == LABEL_S);
        BOOST_ASSERT(by->label == LABEL_S);
        BOOST_ASSERT(bx != by);

        alternating_path_t path = trace_alternating_paths(x, y);

        // Check whether the path starts and ends in the same blossom.
        blossom_t* b1 = vertex_top_blossom[path.front().first];
        blossom_t* b2 = vertex_top_blossom[path.back().second];
        if (b1 == b2)
        {
            make_blossom(path);
            return false;
        }
        else
        {
            // Remove labels the two alternating trees on the augmenting path.
            remove_alternating_tree(bx->tree_root, by->tree_root);

            // Augment matching.
            augment_matching(path);
            return true;
        }
    }

    /**
     * Scan incident edges of newly labeled S-vertices.
     *
     * @return true if the matching was augmented; otherwise false.
     */
    bool scan_new_s_vertices()
    {
        while (!scan_queue.empty())
        {
            vertex_t x = scan_queue.front();
            scan_queue.pop_front();

            BOOST_ASSERT(vertex_top_blossom[x]->label == LABEL_S);

            for (const edge_t& e : make_iterator_range(out_edges(x, *g)))
            {
                BOOST_ASSERT(source(e, *g) == x);
                vertex_t y = target(e, *g);

                // Note: top level blossom of x may change during this loop,
                // so we must look it up on each pass.
                blossom_t* bx = vertex_top_blossom[x];
                blossom_t* by = vertex_top_blossom[y];

                // Ignore internal edges in blossom.
                if (bx == by)
                    continue;

                // Ignore edges with negative slack to prevent numeric overflow.
                if (get(edge_weights, e) < 0)
                    continue;

                weight_t slack = edge_slack(e);
                if (slack <= 0)
                {
                    // Tight edge.
                    if (by->label == LABEL_NONE)
                        extend_tree_s_to_t(x, y);
                    else if (by->label == LABEL_S)
                    {
                        bool augmented = add_s_to_s_edge(x, y);
                        if (augmented)
                            return true;
                    }
                }
                else
                {
                    // Track non-tight edges between S-blossoms.
                    if (by->label == LABEL_S)
                        add_delta3_edge(*bx, e, slack);
                }

                // Track all edges between S-blossom and non-S-blossom.
                if (by->label != LABEL_S)
                    add_delta2_edge(y, e, slack);
            }
        }

        return false;
    }

    /** Calculate a delta step in the dual LPP problem. */
    delta_step_t calc_dual_delta_step()
    {
        delta_step_t delta{};

        // Compute delta1: minimum dual variable of any S-vertex.
        delta.kind = 1;
        delta.value = (std::numeric_limits<weight_t>::max)();
        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            if (vertex_top_blossom[x]->label == LABEL_S)
                delta.value = (std::min)(delta.value, vertex_dual[x]);
        }

        // Compute delta2: minimum slack of edge from S-vertex to unlabeled.
        least_slack_edge_t best_edge = get_least_slack_delta2_edge();
        if (best_edge.edge.has_value() && (best_edge.slack <= delta.value))
        {
            delta.kind = 2;
            delta.edge = *best_edge.edge;
            delta.value = best_edge.slack;
        }

        // Compute delta3: half minimum slack of edge between S-blossoms.
        best_edge = get_least_slack_delta3_edge();
        if (best_edge.edge.has_value() && (best_edge.slack / 2 <= delta.value))
        {
            delta.kind = 3;
            delta.edge = *best_edge.edge;
            delta.value = best_edge.slack / 2;
        }

        // Compute delta4: half minimum dual of a top-level T-blossom.
        for (nontrivial_blossom_t& blossom : nontrivial_blossom)
        {
            if ((!blossom.parent)
                && (blossom.label == LABEL_T)
                && (blossom.dual_var / 2 <= delta.value))
            {
                delta.kind = 4;
                delta.blossom = &blossom;
                delta.value = blossom.dual_var / 2;
            }
        }

        return delta;
    }

    /** Apply a delta step to the dual LPP variables. */
    void apply_delta_step(weight_t delta)
    {
        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            blossom_t* b = vertex_top_blossom[x];
            if (b->label == LABEL_S)
                vertex_dual[x] -= delta;
            else if (b->label == LABEL_T)
                vertex_dual[x] += delta;
        }

        for (nontrivial_blossom_t& blossom : nontrivial_blossom)
        {
            if (!blossom.parent)
            {
                if (blossom.label == LABEL_S)
                    blossom.dual_var += 2 * delta;
                else if (blossom.label == LABEL_T)
                    blossom.dual_var -= 2 * delta;
            }
        }
    }

    /**
     * Run one stage of the matching algorithm.
     *
     * @return True if the matching was successfully augmented;
     *         false if no further improvement is possible.
     */
    bool run_stage()
    {
        // Run substages.
        while (true) {
            bool augmented = scan_new_s_vertices();
            if (augmented)
                return true;

            delta_step_t delta = calc_dual_delta_step();
            apply_delta_step(delta.value);

            if (delta.kind == 2)
            {
                vertex_t x = source(delta.edge, *g);
                vertex_t y = target(delta.edge, *g);
                if (vertex_top_blossom[x]->label != LABEL_S)
                    std::swap(x, y);
                extend_tree_s_to_t(x, y);
            }
            else if (delta.kind == 3)
            {
                vertex_t x = source(delta.edge, *g);
                vertex_t y = target(delta.edge, *g);
                augmented = add_s_to_s_edge(x, y);
                if (augmented)
                    return true;
            }
            else if (delta.kind == 4)
            {
                BOOST_ASSERT(delta.blossom);
                expand_t_blossom(delta.blossom);
            }
            else
            {
                // No further improvement possible.
                BOOST_ASSERT(delta.kind == 1);
                return false;
            }
        }
    }

    /** Run the matching algorithm. */
    void run()
    {
        // Assign label S to all vertices and put them in the queue.
        for (vertex_t x : make_iterator_range(vertices(*g)))
        {
            BOOST_ASSERT(vertex_mate[x] == null_vertex());
            blossom_t* bx = vertex_top_blossom[x];
            BOOST_ASSERT(bx->label == LABEL_NONE);
            BOOST_ASSERT(bx->base_vertex == x);
            bx->label = LABEL_S;
            bx->tree_edge = std::make_pair(null_vertex(), x);
            bx->tree_root = x;
            scan_queue.push_back(x);
        }

        // Improve the solution until no further improvement is possible.
        while (run_stage()) ;

        // Clear temporary data structures.
        scan_queue.clear();
        clear_best_edge();
    }

    /** Copy matching to specified map. */
    template <typename MateMap>
    void extract_matching(MateMap mate)
    {
        for (vertex_t x : make_iterator_range(vertices(*g)))
            put(mate, x, vertex_mate[x]);
    }
};

} // namespace detail
} // namespace graph

/**
 * Compute a maximum-weighted matching in an undirected weighted graph.
 *
 * This function takes time O(V^3).
 * This function uses memory size O(V + E).
 *
 * @param g         Input graph.
 *                  The graph type must be a model of VertexListGraph
 *                  and EdgeListGraph and IncidenceGraph.
 *                  The graph must be undirected.
 *                  The graph may not contain parallel edges.
 *
 * @param mate      ReadWritePropertyMap, mapping vertices to vertices.
 *                  This map returns the result of the computation.
 *                  For each vertex "v", "mate[v]" is the vertex that is
 *                  matched to "v", or "null_vertex()" if "v" is not matched.
 *
 * @param vm        ReadablePropertyMap, mapping vertices to indexes
 *                  in range [0, num_vertices(g)).
 *
 * @param weights   ReadablePropertyMap, mapping edges to edge weights.
 *                  Edge weights must be integers or floating point values.
 *
 * @throw boost::bad_graph  If the input graph is not valid.
 */
template <typename Graph, typename MateMap, typename VertexIndexMap,
    typename EdgeWeightMap>
void maximum_weighted_matching(
    const Graph& g, MateMap mate, VertexIndexMap vm, EdgeWeightMap weights)
{
    BOOST_CONCEPT_ASSERT((VertexListGraphConcept<Graph>));
    BOOST_CONCEPT_ASSERT((EdgeListGraphConcept<Graph>));
    BOOST_CONCEPT_ASSERT((IncidenceGraphConcept<Graph>));
    BOOST_STATIC_ASSERT(is_undirected_graph<Graph>::value);

    using vertex_t = typename graph_traits<Graph>::vertex_descriptor;
    using edge_t = typename graph_traits<Graph>::edge_descriptor;
    BOOST_CONCEPT_ASSERT((ReadWritePropertyMapConcept<MateMap, vertex_t>));
    BOOST_CONCEPT_ASSERT(
        (ReadablePropertyMapConcept<VertexIndexMap, vertex_t>));
    BOOST_CONCEPT_ASSERT((ReadablePropertyMapConcept<EdgeWeightMap, edge_t>));

    graph::detail::check_vertex_index_range(g, vm);
    graph::detail::check_maximum_weighted_matching_edge_weights(g, weights);

    graph::detail::maximum_weighted_matching_context<
        Graph, VertexIndexMap, EdgeWeightMap>
        matching(g, vm, weights);
    matching.run();
    matching.extract_matching(mate);
}

/**
 * Compute a maximum-weighted matching in an undirected weighted graph.
 *
 * This overloaded function obtains edge weights from "get(edge_weight, g)".
 */
template <typename Graph, typename MateMap, typename VertexIndexMap>
inline void maximum_weighted_matching(
    const Graph& g, MateMap mate, VertexIndexMap vm)
{
    maximum_weighted_matching(g, mate, vm, get(edge_weight, g));
}

/**
 * Compute a maximum-weighted matching in an undirected weighted graph.
 *
 * This overloaded function obtains vertex indices from "get(vertex_index, g)"
 * and edge weights from "get(edge_weight, g)".
 */
template <typename Graph, typename MateMap>
inline void maximum_weighted_matching(const Graph& g, MateMap mate)
{
    maximum_weighted_matching(g, mate, get(vertex_index, g));
}

} // namespace boost

#endif // BOOST_GRAPH_MAXIMUM_WEIGHTED_MATCHING_HPP
