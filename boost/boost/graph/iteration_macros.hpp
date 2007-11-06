//=======================================================================
// Copyright 2001 Indiana University
// Author: Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#ifndef BOOST_GRAPH_ITERATION_MACROS_HPP
#define BOOST_GRAPH_ITERATION_MACROS_HPP

#define BGL_CAT(x,y) x ## y
#define BGL_FIRST(linenum) BGL_CAT(bgl_first_,linenum)
#define BGL_LAST(linenum) BGL_CAT(bgl_last_,linenum)

/*
  BGL_FORALL_VERTICES_T(v, g, graph_t)  // This is on line 9
  expands to the following, but all on the same line

  for (typename boost::graph_traits<graph_t>::vertex_iterator 
           bgl_first_9 = vertices(g).first, bgl_last_9 = vertices(g).second;
       bgl_first_9 != bgl_last_9; bgl_first_9 = bgl_last_9)
    for (typename boost::graph_traits<graph_t>::vertex_descriptor v;
         bgl_first_9 != bgl_last ? (v = *bgl_first_9, true) : false;
         ++bgl_first_9)

  The purpose of having two for-loops is just to provide a place to
  declare both the iterator and value variables. There is really only
  one loop. The stopping condition gets executed two more times than it
  usually would be, oh well. The reason for the bgl_first_9 = bgl_last_9
  in the outer for-loop is in case the user puts a break statement
  in the inner for-loop.

  The other macros work in a similar fashion.

  Use the _T versions when the graph type is a template parameter or
  dependent on a template parameter. Otherwise use the non _T versions.
  
 */


#define BGL_FORALL_VERTICES_T(VNAME, GNAME, GraphType) \
for (typename boost::graph_traits<GraphType>::vertex_iterator \
  BGL_FIRST(__LINE__) = vertices(GNAME).first, BGL_LAST(__LINE__) = vertices(GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
  for (typename boost::graph_traits<GraphType>::vertex_descriptor VNAME; \
    BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (VNAME = *BGL_FIRST(__LINE__), true):false; \
     ++BGL_FIRST(__LINE__))

#define BGL_FORALL_VERTICES(VNAME, GNAME, GraphType) \
for (boost::graph_traits<GraphType>::vertex_iterator \
  BGL_FIRST(__LINE__) = vertices(GNAME).first, BGL_LAST(__LINE__) = vertices(GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
  for (boost::graph_traits<GraphType>::vertex_descriptor VNAME; \
    BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (VNAME = *BGL_FIRST(__LINE__), true):false; \
     ++BGL_FIRST(__LINE__))

#define BGL_FORALL_EDGES_T(ENAME, GNAME, GraphType) \
for (typename boost::graph_traits<GraphType>::edge_iterator \
  BGL_FIRST(__LINE__) = edges(GNAME).first, BGL_LAST(__LINE__) = edges(GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
  for (typename boost::graph_traits<GraphType>::edge_descriptor ENAME; \
    BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (ENAME = *BGL_FIRST(__LINE__), true):false; \
     ++BGL_FIRST(__LINE__))

#define BGL_FORALL_EDGES(ENAME, GNAME, GraphType) \
for (boost::graph_traits<GraphType>::edge_iterator \
  BGL_FIRST(__LINE__) = edges(GNAME).first, BGL_LAST(__LINE__) = edges(GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
  for (boost::graph_traits<GraphType>::edge_descriptor ENAME; \
     BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (ENAME = *BGL_FIRST(__LINE__), true):false; \
     ++BGL_FIRST(__LINE__))

#define BGL_FORALL_ADJ_T(UNAME, VNAME, GNAME, GraphType) \
for (typename boost::graph_traits<GraphType>::adjacency_iterator \
  BGL_FIRST(__LINE__) = adjacent_vertices(UNAME, GNAME).first,\
  BGL_LAST(__LINE__) = adjacent_vertices(UNAME, GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
for (typename boost::graph_traits<GraphType>::vertex_descriptor VNAME; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (VNAME = *BGL_FIRST(__LINE__), true) : false; \
   ++BGL_FIRST(__LINE__))

#define BGL_FORALL_ADJ(UNAME, VNAME, GNAME, GraphType) \
for (boost::graph_traits<GraphType>::adjacency_iterator \
  BGL_FIRST(__LINE__) = adjacent_vertices(UNAME, GNAME).first,\
  BGL_LAST(__LINE__) = adjacent_vertices(UNAME, GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
for (boost::graph_traits<GraphType>::vertex_descriptor VNAME; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (VNAME = *BGL_FIRST(__LINE__), true) : false; \
   ++BGL_FIRST(__LINE__))

#define BGL_FORALL_OUTEDGES_T(UNAME, ENAME, GNAME, GraphType) \
for (typename boost::graph_traits<GraphType>::out_edge_iterator \
  BGL_FIRST(__LINE__) = out_edges(UNAME, GNAME).first,\
  BGL_LAST(__LINE__) = out_edges(UNAME, GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
for (typename boost::graph_traits<GraphType>::edge_descriptor ENAME; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (ENAME = *BGL_FIRST(__LINE__), true) : false; \
   ++BGL_FIRST(__LINE__))

#define BGL_FORALL_OUTEDGES(UNAME, ENAME, GNAME, GraphType) \
for (boost::graph_traits<GraphType>::out_edge_iterator \
  BGL_FIRST(__LINE__) = out_edges(UNAME, GNAME).first,\
  BGL_LAST(__LINE__) = out_edges(UNAME, GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
for (boost::graph_traits<GraphType>::edge_descriptor ENAME; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (ENAME = *BGL_FIRST(__LINE__), true) : false; \
   ++BGL_FIRST(__LINE__))

#define BGL_FORALL_INEDGES_T(UNAME, ENAME, GNAME, GraphType) \
for (typename boost::graph_traits<GraphType>::in_edge_iterator \
  BGL_FIRST(__LINE__) = in_edges(UNAME, GNAME).first,\
  BGL_LAST(__LINE__) = in_edges(UNAME, GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
for (typename boost::graph_traits<GraphType>::edge_descriptor ENAME; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (ENAME = *BGL_FIRST(__LINE__), true) : false; \
   ++BGL_FIRST(__LINE__))

#define BGL_FORALL_INEDGES(UNAME, ENAME, GNAME, GraphType) \
for (boost::graph_traits<GraphType>::in_edge_iterator \
  BGL_FIRST(__LINE__) = in_edges(UNAME, GNAME).first,\
  BGL_LAST(__LINE__) = in_edges(UNAME, GNAME).second; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__); BGL_FIRST(__LINE__) = BGL_LAST(__LINE__)) \
for (boost::graph_traits<GraphType>::edge_descriptor ENAME; \
  BGL_FIRST(__LINE__) != BGL_LAST(__LINE__) ? (ENAME = *BGL_FIRST(__LINE__), true) : false; \
   ++BGL_FIRST(__LINE__))

#endif // BOOST_GRAPH_ITERATION_MACROS_HPP
