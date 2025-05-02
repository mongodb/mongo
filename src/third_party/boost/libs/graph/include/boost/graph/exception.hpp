//=======================================================================
// Copyright 2002 Indiana University.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#ifndef BOOST_GRAPH_EXCEPTION_HPP
#define BOOST_GRAPH_EXCEPTION_HPP

#include <stdexcept>
#include <string>

#include <boost/config.hpp>

namespace boost
{

struct BOOST_SYMBOL_VISIBLE bad_graph : public std::invalid_argument
{
    bad_graph(const std::string& what_arg) : std::invalid_argument(what_arg) {}
};

struct BOOST_SYMBOL_VISIBLE not_a_dag : public bad_graph
{
    not_a_dag() : bad_graph("The graph must be a DAG.") {}
};

struct BOOST_SYMBOL_VISIBLE negative_edge : public bad_graph
{
    negative_edge()
    : bad_graph("The graph may not contain an edge with negative weight.")
    {
    }
};

struct BOOST_SYMBOL_VISIBLE negative_cycle : public bad_graph
{
    negative_cycle() : bad_graph("The graph may not contain negative cycles.")
    {
    }
};

struct BOOST_SYMBOL_VISIBLE not_connected : public bad_graph
{
    not_connected() : bad_graph("The graph must be connected.") {}
};

struct BOOST_SYMBOL_VISIBLE not_complete : public bad_graph
{
    not_complete() : bad_graph("The graph must be complete.") {}
};

struct BOOST_SYMBOL_VISIBLE graph_exception : public std::exception
{
    ~graph_exception() throw() BOOST_OVERRIDE {}
    const char* what() const throw() BOOST_OVERRIDE = 0;
};

struct BOOST_SYMBOL_VISIBLE bad_parallel_edge : public graph_exception
{
    std::string from;
    std::string to;
    mutable std::string statement;
    bad_parallel_edge(const std::string& i, const std::string& j)
    : from(i), to(j)
    {
    }

    ~bad_parallel_edge() throw() BOOST_OVERRIDE {}
    const char* what() const throw() BOOST_OVERRIDE
    {
        if (statement.empty())
            statement = std::string("Failed to add parallel edge: (") + from
                + "," + to + ")\n";

        return statement.c_str();
    }
};

struct BOOST_SYMBOL_VISIBLE directed_graph_error : public graph_exception
{
    ~directed_graph_error() throw() BOOST_OVERRIDE {}
    const char* what() const throw() BOOST_OVERRIDE
    {
        return "read_graphviz: "
               "Tried to read a directed graph into an undirected graph.";
    }
};

struct BOOST_SYMBOL_VISIBLE undirected_graph_error : public graph_exception
{
    ~undirected_graph_error() throw() BOOST_OVERRIDE {}
    const char* what() const throw() BOOST_OVERRIDE
    {
        return "read_graphviz: "
               "Tried to read an undirected graph into a directed graph.";
    }
};

} // namespace boost

#endif // BOOST_GRAPH_EXCEPTION_HPP
