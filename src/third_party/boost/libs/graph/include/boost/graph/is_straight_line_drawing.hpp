//=======================================================================
// Copyright 2007 Aaron Windsor
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
#ifndef __IS_STRAIGHT_LINE_DRAWING_HPP__
#define __IS_STRAIGHT_LINE_DRAWING_HPP__

#include <boost/config.hpp>
#include <boost/next_prior.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/tuple/tuple_comparison.hpp>
#include <boost/property_map/property_map.hpp>
#include <boost/graph/properties.hpp>
#include <boost/graph/planar_detail/bucket_sort.hpp>

#include <boost/geometry/algorithms/crosses.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <boost/geometry/core/coordinate_type.hpp>

#include <boost/numeric/conversion/cast.hpp>

#include <algorithm>
#include <vector>
#include <map>

namespace boost
{
// Overload of make from Boost.Geometry.
template<typename Geometry, typename Graph, typename GridPositionMap>
Geometry make(typename graph_traits<Graph>::edge_descriptor e,
              Graph const &g,
              GridPositionMap const &drawing)
{
    auto e_source(source(e, g));
    auto e_target(target(e, g));
    using Float = typename geometry::coordinate_type<Geometry>::type;
    return {{numeric_cast<Float>(drawing[e_source].x), numeric_cast<Float>(drawing[e_source].y)},
            {numeric_cast<Float>(drawing[e_target].x), numeric_cast<Float>(drawing[e_target].y)}};
}

// Overload of crosses from Boost.Geometry.
template<typename Graph, typename GridPositionMap>
bool crosses(typename graph_traits<Graph>::edge_descriptor e,
             typename graph_traits<Graph>::edge_descriptor f,
             Graph const &g,
             GridPositionMap const &drawing)
{
    using geometry::crosses;
    using geometry::model::linestring;
    using geometry::model::d2::point_xy;
    using linestring2d = geometry::model::linestring<geometry::model::d2::point_xy<double>>;
    return crosses(make<linestring2d>(e, g, drawing),
                   make<linestring2d>(f, g, drawing));
}

template < typename Graph, typename GridPositionMap, typename VertexIndexMap >
bool is_straight_line_drawing(
    const Graph& g, GridPositionMap drawing, VertexIndexMap)
{

    typedef typename graph_traits< Graph >::vertex_descriptor vertex_t;
    typedef typename graph_traits< Graph >::edge_descriptor edge_t;
    typedef typename graph_traits< Graph >::edge_iterator edge_iterator_t;

    typedef std::size_t x_coord_t;
    typedef std::size_t y_coord_t;
    typedef boost::tuple< edge_t, x_coord_t, y_coord_t > edge_event_t;
    typedef typename std::vector< edge_event_t > edge_event_queue_t;

    typedef tuple< y_coord_t, y_coord_t, x_coord_t, x_coord_t >
        active_map_key_t;
    typedef edge_t active_map_value_t;
    typedef std::map< active_map_key_t, active_map_value_t > active_map_t;
    typedef typename active_map_t::iterator active_map_iterator_t;

    edge_event_queue_t edge_event_queue;
    active_map_t active_edges;

    edge_iterator_t ei, ei_end;
    for (boost::tie(ei, ei_end) = edges(g); ei != ei_end; ++ei)
    {
        edge_t e(*ei);
        vertex_t s(source(e, g));
        vertex_t t(target(e, g));
        edge_event_queue.push_back(
            make_tuple(e, static_cast< std::size_t >(drawing[s].x),
                static_cast< std::size_t >(drawing[s].y)));
        edge_event_queue.push_back(
            make_tuple(e, static_cast< std::size_t >(drawing[t].x),
                static_cast< std::size_t >(drawing[t].y)));
    }

    // Order by edge_event_queue by first, then second coordinate
    // (bucket_sort is a stable sort.)
    bucket_sort(edge_event_queue.begin(), edge_event_queue.end(),
        property_map_tuple_adaptor< edge_event_t, 2 >());

    bucket_sort(edge_event_queue.begin(), edge_event_queue.end(),
        property_map_tuple_adaptor< edge_event_t, 1 >());

    typedef typename edge_event_queue_t::iterator event_queue_iterator_t;
    event_queue_iterator_t itr_end = edge_event_queue.end();
    for (event_queue_iterator_t itr = edge_event_queue.begin(); itr != itr_end;
         ++itr)
    {
        edge_t e(get< 0 >(*itr));
        vertex_t source_v(source(e, g));
        vertex_t target_v(target(e, g));
        if (drawing[source_v].y > drawing[target_v].y)
            std::swap(source_v, target_v);

        active_map_key_t key(get(drawing, source_v).y, get(drawing, target_v).y,
            get(drawing, source_v).x, get(drawing, target_v).x);

        active_map_iterator_t a_itr = active_edges.find(key);
        if (a_itr == active_edges.end())
        {
            active_edges[key] = e;
        }
        else
        {
            active_map_iterator_t before, after;
            if (a_itr == active_edges.begin())
                before = active_edges.end();
            else
                before = prior(a_itr);
            after = boost::next(a_itr);

            if (before != active_edges.end())
            {
                edge_t f = before->second;
                if (crosses(e, f, g, drawing))
                    return false;
            }

            if (after != active_edges.end())
            {
                edge_t f = after->second;
                if (crosses(e, f, g, drawing))
                    return false;
            }

            active_edges.erase(a_itr);
        }
    }

    return true;
}

template < typename Graph, typename GridPositionMap >
bool is_straight_line_drawing(const Graph& g, GridPositionMap drawing)
{
    return is_straight_line_drawing(g, drawing, get(vertex_index, g));
}

}

#endif // __IS_STRAIGHT_LINE_DRAWING_HPP__
