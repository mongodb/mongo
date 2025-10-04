//            Copyright Daniel Trebbien 2010.
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE_1_0.txt or the copy at
//         http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_GRAPH_STOER_WAGNER_MIN_CUT_HPP
#define BOOST_GRAPH_STOER_WAGNER_MIN_CUT_HPP 1

#include <boost/assert.hpp>
#include <set>
#include <vector>
#include <boost/concept_check.hpp>
#include <boost/concept/assert.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/buffer_concepts.hpp>
#include <boost/graph/exception.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/maximum_adjacency_search.hpp>
#include <boost/graph/named_function_params.hpp>
#include <boost/graph/one_bit_color_map.hpp>
#include <boost/graph/detail/d_ary_heap.hpp>
#include <boost/property_map/property_map.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/utility/result_of.hpp>
#include <boost/graph/iteration_macros.hpp>

namespace boost
{

namespace detail
{
    /**
     * \brief Performs a phase of the Stoer-Wagner min-cut algorithm
     *
     * Performs a phase of the Stoer-Wagner min-cut algorithm.
     *
     * As described by Stoer & Wagner (1997), a phase is simply a maximum
     * adjacency search (also called a maximum cardinality search), which
     * results in the selection of two vertices \em s and \em t, and, as a side
     * product, a minimum <em>s</em>-<em>t</em> cut of the input graph. Here,
     * the input graph is basically \p g, but some vertices are virtually
     * assigned to others as a way of viewing \p g as a graph with some sets of
     * vertices merged together.
     *
     * This implementation is a translation of pseudocode by Professor Uri
     * Zwick, School of Computer Science, Tel Aviv University.
     *
     * \pre \p g is a connected, undirected graph
     * \param[in] g the input graph
     * \param[in] assignments a read/write property map from each vertex to the
     *                        vertex that it is assigned to
     * \param[in] assignedVertices a list of vertices that are assigned to
     *                             others
     * \param[in] weights a readable property map from each edge to its
     *                    weight (a non-negative value)
     * \param[out] pq a keyed, updatable max-priority queue
     * \returns a tuple (\em s, \em t, \em w) of the "<em>s</em>" and
     *          "<em>t</em>" of the minimum <em>s</em>-<em>t</em> cut and the
     *          cut weight \em w of the minimum <em>s</em>-<em>t</em> cut.
     * \see http://www.cs.tau.ac.il/~zwick/grad-algo-08/gmc.pdf
     *
     * \author Daniel Trebbien
     * \date 2010-09-11
     */
    template < class UndirectedGraph, class VertexAssignmentMap,
        class WeightMap, class KeyedUpdatablePriorityQueue >
    boost::tuple<
        typename boost::graph_traits< UndirectedGraph >::vertex_descriptor,
        typename boost::graph_traits< UndirectedGraph >::vertex_descriptor,
        typename boost::property_traits< WeightMap >::value_type >
    stoer_wagner_phase(const UndirectedGraph& g,
        VertexAssignmentMap assignments,
        const std::set< typename boost::graph_traits<
            UndirectedGraph >::vertex_descriptor >& assignedVertices,
        WeightMap weights, KeyedUpdatablePriorityQueue& pq)
    {
        typedef
            typename boost::graph_traits< UndirectedGraph >::vertex_descriptor
                vertex_descriptor;
        typedef typename boost::property_traits< WeightMap >::value_type
            weight_type;

        BOOST_ASSERT(pq.empty());
        typename KeyedUpdatablePriorityQueue::key_map keys = pq.keys();

        BGL_FORALL_VERTICES_T(v, g, UndirectedGraph)
        {
            if (v == get(assignments, v))
            { // foreach u \in V do
                put(keys, v, weight_type(0));

                pq.push(v);
            }
        }

        BOOST_ASSERT(pq.size() >= 2);

        vertex_descriptor s
            = boost::graph_traits< UndirectedGraph >::null_vertex();
        vertex_descriptor t
            = boost::graph_traits< UndirectedGraph >::null_vertex();
        weight_type w;
        while (!pq.empty())
        { // while PQ \neq {} do
            const vertex_descriptor u = pq.top(); // u = extractmax(PQ)
            w = get(keys, u);
            pq.pop();

            s = t;
            t = u;

            BGL_FORALL_OUTEDGES_T(u, e, g, UndirectedGraph)
            { // foreach (u, v) \in E do
                const vertex_descriptor v = get(assignments, target(e, g));

                if (pq.contains(v))
                { // if v \in PQ then
                    put(keys, v,
                        get(keys, v)
                            + get(weights,
                                e)); // increasekey(PQ, v, wA(v) + w(u, v))
                    pq.update(v);
                }
            }

            typename std::set< vertex_descriptor >::const_iterator
                assignedVertexIt,
                assignedVertexEnd = assignedVertices.end();
            for (assignedVertexIt = assignedVertices.begin();
                 assignedVertexIt != assignedVertexEnd; ++assignedVertexIt)
            {
                const vertex_descriptor uPrime = *assignedVertexIt;

                if (get(assignments, uPrime) == u)
                {
                    BGL_FORALL_OUTEDGES_T(uPrime, e, g, UndirectedGraph)
                    { // foreach (u, v) \in E do
                        const vertex_descriptor v
                            = get(assignments, target(e, g));

                        if (pq.contains(v))
                        { // if v \in PQ then
                            put(keys, v,
                                get(keys, v)
                                    + get(weights, e)); // increasekey(PQ, v,
                                                        // wA(v) + w(u, v))
                            pq.update(v);
                        }
                    }
                }
            }
        }

        return boost::make_tuple(s, t, w);
    }

    /**
     * \brief Computes a min-cut of the input graph
     *
     * Computes a min-cut of the input graph using the Stoer-Wagner algorithm.
     *
     * \pre \p g is a connected, undirected graph
     * \pre <code>pq.empty()</code>
     * \param[in] g the input graph
     * \param[in] weights a readable property map from each edge to its weight
     * (a non-negative value) \param[out] parities a writable property map from
     * each vertex to a bool type object for distinguishing the two vertex sets
     * of the min-cut \param[out] assignments a read/write property map from
     * each vertex to a \c vertex_descriptor object. This map serves as work
     * space, and no particular meaning should be derived from property values
     *     after completion of the algorithm.
     * \param[out] pq a keyed, updatable max-priority queue
     * \returns the cut weight of the min-cut
     * \see
     * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.114.6687&rep=rep1&type=pdf
     * \see
     * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.31.614&rep=rep1&type=pdf
     *
     * \author Daniel Trebbien
     * \date 2010-09-11
     */
    template < class UndirectedGraph, class WeightMap, class ParityMap,
        class VertexAssignmentMap, class KeyedUpdatablePriorityQueue,
        class IndexMap >
    typename boost::property_traits< WeightMap >::value_type
    stoer_wagner_min_cut(const UndirectedGraph& g, WeightMap weights,
        ParityMap parities, VertexAssignmentMap assignments,
        KeyedUpdatablePriorityQueue& pq, IndexMap index_map)
    {
        typedef
            typename boost::graph_traits< UndirectedGraph >::vertex_descriptor
                vertex_descriptor;
        typedef typename boost::property_traits< WeightMap >::value_type
            weight_type;
        typedef
            typename boost::graph_traits< UndirectedGraph >::vertices_size_type
                vertices_size_type;
        typedef typename boost::property_traits< ParityMap >::value_type
            parity_type;

        vertices_size_type n = num_vertices(g);

        std::set< vertex_descriptor > assignedVertices;

        // initialize `assignments` (all vertices are initially assigned to
        // themselves)
        BGL_FORALL_VERTICES_T(v, g, UndirectedGraph) { put(assignments, v, v); }

        vertex_descriptor s, t;
        weight_type bestW;

        boost::tie(s, t, bestW) = boost::detail::stoer_wagner_phase(
            g, assignments, assignedVertices, weights, pq);
        BOOST_ASSERT(s != t);
        BGL_FORALL_VERTICES_T(v, g, UndirectedGraph)
        {
            put(parities, v, parity_type(v == t ? 1 : 0));
        }
        put(assignments, t, s);
        assignedVertices.insert(t);
        --n;

        for (; n >= 2; --n)
        {
            weight_type w;
            boost::tie(s, t, w) = boost::detail::stoer_wagner_phase(
                g, assignments, assignedVertices, weights, pq);
            BOOST_ASSERT(s != t);

            if (w < bestW)
            {
                BGL_FORALL_VERTICES_T(v, g, UndirectedGraph)
                {
                    put(parities, v,
                        parity_type(get(assignments, v) == t ? 1 : 0));

                    if (get(assignments, v)
                        == t) // all vertices that were assigned to t are now
                              // assigned to s
                        put(assignments, v, s);
                }

                bestW = w;
            }
            else
            {
                BGL_FORALL_VERTICES_T(v, g, UndirectedGraph)
                {
                    if (get(assignments, v)
                        == t) // all vertices that were assigned to t are now
                              // assigned to s
                        put(assignments, v, s);
                }
            }
            put(assignments, t, s);
            assignedVertices.insert(t);
        }

        BOOST_ASSERT(pq.empty());

        return bestW;
    }
} // end `namespace detail` within `namespace boost`

template < class UndirectedGraph, class WeightMap, class ParityMap,
    class VertexAssignmentMap, class KeyedUpdatablePriorityQueue,
    class IndexMap >
typename boost::property_traits< WeightMap >::value_type stoer_wagner_min_cut(
    const UndirectedGraph& g, WeightMap weights, ParityMap parities,
    VertexAssignmentMap assignments, KeyedUpdatablePriorityQueue& pq,
    IndexMap index_map)
{
    BOOST_CONCEPT_ASSERT((boost::IncidenceGraphConcept< UndirectedGraph >));
    BOOST_CONCEPT_ASSERT((boost::VertexListGraphConcept< UndirectedGraph >));
    typedef typename boost::graph_traits< UndirectedGraph >::vertex_descriptor
        vertex_descriptor;
    typedef typename boost::graph_traits< UndirectedGraph >::vertices_size_type
        vertices_size_type;
    typedef typename boost::graph_traits< UndirectedGraph >::edge_descriptor
        edge_descriptor;
    BOOST_CONCEPT_ASSERT((boost::Convertible<
        typename boost::graph_traits< UndirectedGraph >::directed_category,
        boost::undirected_tag >));
    BOOST_CONCEPT_ASSERT(
        (boost::ReadablePropertyMapConcept< WeightMap, edge_descriptor >));
    // typedef typename boost::property_traits<WeightMap>::value_type
    // weight_type;
    BOOST_CONCEPT_ASSERT(
        (boost::WritablePropertyMapConcept< ParityMap, vertex_descriptor >));
    // typedef typename boost::property_traits<ParityMap>::value_type
    // parity_type;
    BOOST_CONCEPT_ASSERT(
        (boost::ReadWritePropertyMapConcept< VertexAssignmentMap,
            vertex_descriptor >));
    BOOST_CONCEPT_ASSERT((boost::Convertible< vertex_descriptor,
        typename boost::property_traits< VertexAssignmentMap >::value_type >));
    BOOST_CONCEPT_ASSERT(
        (boost::KeyedUpdatableQueueConcept< KeyedUpdatablePriorityQueue >));

    vertices_size_type n = num_vertices(g);
    if (n < 2)
        throw boost::bad_graph(
            "the input graph must have at least two vertices.");
    else if (!pq.empty())
        throw std::invalid_argument(
            "the max-priority queue must be empty initially.");

    return detail::stoer_wagner_min_cut(
        g, weights, parities, assignments, pq, index_map);
}

namespace graph
{
    namespace detail
    {
        template < class UndirectedGraph, class WeightMap >
        struct stoer_wagner_min_cut_impl
        {
            typedef typename boost::property_traits< WeightMap >::value_type
                result_type;
            template < typename ArgPack >
            result_type operator()(const UndirectedGraph& g, WeightMap weights,
                const ArgPack& arg_pack) const
            {
                using namespace boost::graph::keywords;
                typedef typename boost::graph_traits<
                    UndirectedGraph >::vertex_descriptor vertex_descriptor;
                typedef typename boost::property_traits< WeightMap >::value_type
                    weight_type;

                typedef boost::detail::make_priority_queue_from_arg_pack_gen<
                    boost::graph::keywords::tag::max_priority_queue,
                    weight_type, vertex_descriptor,
                    std::greater< weight_type > >
                    gen_type;

                gen_type gen(
                    choose_param(get_param(arg_pack, boost::distance_zero_t()),
                        weight_type(0)));

                typename boost::result_of< gen_type(
                    const UndirectedGraph&, const ArgPack&) >::type pq
                    = gen(g, arg_pack);

                boost::dummy_property_map dummy_prop;
                return boost::stoer_wagner_min_cut(g, weights,
                    arg_pack[_parity_map | dummy_prop],
                    boost::detail::make_property_map_from_arg_pack_gen<
                        tag::vertex_assignment_map, vertex_descriptor >(
                        vertex_descriptor())(g, arg_pack),
                    pq,
                    boost::detail::override_const_property(
                        arg_pack, _vertex_index_map, g, vertex_index));
            }
        };
    }
    BOOST_GRAPH_MAKE_FORWARDING_FUNCTION(stoer_wagner_min_cut, 2, 4)
}

// Named parameter interface
BOOST_GRAPH_MAKE_OLD_STYLE_PARAMETER_FUNCTION(stoer_wagner_min_cut, 2)
namespace graph
{
    // version without IndexMap kept for backwards compatibility
    // (but requires vertex_index_t to be defined in the graph)
    // Place after the macro to avoid compilation errors
    template < class UndirectedGraph, class WeightMap, class ParityMap,
        class VertexAssignmentMap, class KeyedUpdatablePriorityQueue >
    typename boost::property_traits< WeightMap >::value_type
    stoer_wagner_min_cut(const UndirectedGraph& g, WeightMap weights,
        ParityMap parities, VertexAssignmentMap assignments,
        KeyedUpdatablePriorityQueue& pq)
    {

        return stoer_wagner_min_cut(
            g, weights, parities, assignments, pq, get(vertex_index, g));
    }
} // end `namespace graph`
} // end `namespace boost`

#include <boost/graph/iteration_macros_undef.hpp>

#endif // !BOOST_GRAPH_STOER_WAGNER_MIN_CUT_HPP
