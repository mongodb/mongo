// Copyright (C) 2006  Tiago de Paula Peixoto <tiago@forked.de>
// Copyright (C) 2004,2009  The Trustees of Indiana University.
//
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor
//           Jeremiah Willcock
//           Andrew Lumsdaine
//           Tiago de Paula Peixoto

#define BOOST_GRAPH_SOURCE
#include <boost/foreach.hpp>
#include <boost/optional.hpp>
#include <boost/throw_exception.hpp>
#include <boost/graph/graphml.hpp>
#include <boost/graph/dll_import_export.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <map>
#include <string>
#include <vector>

using namespace boost;

namespace
{

class graphml_reader
{
public:
    graphml_reader(mutate_graph& g) : m_g(g) {}

    static boost::property_tree::ptree::path_type path(const std::string& str)
    {
        return boost::property_tree::ptree::path_type(str, '/');
    }

    void get_graphs(const boost::property_tree::ptree& top,
        size_t desired_idx /* or -1 for all */, bool is_root,
        std::vector< const boost::property_tree::ptree* >& result)
    {
        using boost::property_tree::ptree;
        size_t current_idx = 0;
        bool is_first = is_root;
        BOOST_FOREACH (const ptree::value_type& n, top)
        {
            if (n.first == "graph")
            {
                if (current_idx == desired_idx || desired_idx == (size_t)(-1))
                {
                    result.push_back(&n.second);
                    if (is_first)
                    {
                        is_first = false;
                        BOOST_FOREACH (const ptree::value_type& attr, n.second)
                        {
                            if (attr.first != "data")
                                continue;
                            std::string key = attr.second.get< std::string >(
                                path("<xmlattr>/key"));
                            std::string value = attr.second.get_value("");
                            handle_graph_property(key, value);
                        }
                    }

                    get_graphs(n.second, (size_t)(-1), false, result);
                    if (desired_idx != (size_t)(-1))
                        break;
                }
                ++current_idx;
            }
        }
    }

    void run(std::istream& in, size_t desired_idx)
    {
        using boost::property_tree::ptree;
        ptree pt;
        read_xml(in, pt,
            boost::property_tree::xml_parser::no_comments
                | boost::property_tree::xml_parser::trim_whitespace);
        ptree gml = pt.get_child(path("graphml"));
        // Search for attributes
        BOOST_FOREACH (const ptree::value_type& child, gml)
        {
            if (child.first != "key")
                continue;
            std::string id = child.second.get(path("<xmlattr>/id"), "");
            std::string for_ = child.second.get(path("<xmlattr>/for"), "");
            std::string name
                = child.second.get(path("<xmlattr>/attr.name"), "");
            std::string type
                = child.second.get(path("<xmlattr>/attr.type"), "");
            key_kind kind = all_key;
            if (for_ == "graph")
                kind = graph_key;
            else if (for_ == "node")
                kind = node_key;
            else if (for_ == "edge")
                kind = edge_key;
            else if (for_ == "hyperedge")
                kind = hyperedge_key;
            else if (for_ == "port")
                kind = port_key;
            else if (for_ == "endpoint")
                kind = endpoint_key;
            else if (for_ == "all")
                kind = all_key;
            else if (for_ == "graphml")
                kind = graphml_key;
            else
            {
                BOOST_THROW_EXCEPTION(
                    parse_error("Attribute for is not valid: " + for_));
            }
            m_keys[id] = kind;
            m_key_name[id] = name;
            m_key_type[id] = type;
            boost::optional< std::string > default_
                = child.second.get_optional< std::string >(path("default"));
            if (default_)
                m_key_default[id] = default_.get();
        }
        // Search for graphs
        std::vector< const ptree* > graphs;
        handle_graph();
        get_graphs(gml, desired_idx, true, graphs);
        BOOST_FOREACH (const ptree* gr, graphs)
        {
            // Search for nodes
            BOOST_FOREACH (const ptree::value_type& node, *gr)
            {
                if (node.first != "node")
                    continue;
                std::string id
                    = node.second.get< std::string >(path("<xmlattr>/id"));
                handle_vertex(id);
                BOOST_FOREACH (const ptree::value_type& attr, node.second)
                {
                    if (attr.first != "data")
                        continue;
                    std::string key
                        = attr.second.get< std::string >(path("<xmlattr>/key"));
                    std::string value = attr.second.get_value("");
                    handle_node_property(key, id, value);
                }
            }
        }
        BOOST_FOREACH (const ptree* gr, graphs)
        {
            bool default_directed
                = gr->get< std::string >(path("<xmlattr>/edgedefault"))
                == "directed";
            // Search for edges
            BOOST_FOREACH (const ptree::value_type& edge, *gr)
            {
                if (edge.first != "edge")
                    continue;
                std::string source
                    = edge.second.get< std::string >(path("<xmlattr>/source"));
                std::string target
                    = edge.second.get< std::string >(path("<xmlattr>/target"));
                std::string local_directed
                    = edge.second.get(path("<xmlattr>/directed"), "");
                bool is_directed
                    = (local_directed.empty() ? default_directed
                                              : local_directed == "true");
                if (is_directed != m_g.is_directed())
                {
                    if (is_directed)
                    {
                        BOOST_THROW_EXCEPTION(directed_graph_error());
                    }
                    else
                    {
                        BOOST_THROW_EXCEPTION(undirected_graph_error());
                    }
                }
                size_t old_edges_size = m_edge.size();
                handle_edge(source, target);
                BOOST_FOREACH (const ptree::value_type& attr, edge.second)
                {
                    if (attr.first != "data")
                        continue;
                    std::string key
                        = attr.second.get< std::string >(path("<xmlattr>/key"));
                    std::string value = attr.second.get_value("");
                    handle_edge_property(key, old_edges_size, value);
                }
            }
        }
    }

private:
    /// The kinds of keys. Not all of these are supported
    enum key_kind
    {
        graph_key,
        node_key,
        edge_key,
        hyperedge_key,
        port_key,
        endpoint_key,
        all_key,
        graphml_key
    };

    void handle_vertex(const std::string& v)
    {
        bool is_new = false;

        if (m_vertex.find(v) == m_vertex.end())
        {
            m_vertex[v] = m_g.do_add_vertex();
            is_new = true;
        }

        if (is_new)
        {
            std::map< std::string, std::string >::iterator iter;
            for (iter = m_key_default.begin(); iter != m_key_default.end();
                 ++iter)
            {
                if (m_keys[iter->first] == node_key)
                    handle_node_property(iter->first, v, iter->second);
            }
        }
    }

    any get_vertex_descriptor(const std::string& v) { return m_vertex[v]; }

    void handle_edge(const std::string& u, const std::string& v)
    {
        handle_vertex(u);
        handle_vertex(v);

        any source, target;
        source = get_vertex_descriptor(u);
        target = get_vertex_descriptor(v);

        any edge;
        bool added;
        boost::tie(edge, added) = m_g.do_add_edge(source, target);
        if (!added)
        {
            BOOST_THROW_EXCEPTION(bad_parallel_edge(u, v));
        }

        size_t e = m_edge.size();
        m_edge.push_back(edge);

        std::map< std::string, std::string >::iterator iter;
        for (iter = m_key_default.begin(); iter != m_key_default.end(); ++iter)
        {
            if (m_keys[iter->first] == edge_key)
                handle_edge_property(iter->first, e, iter->second);
        }
    }

    void handle_graph()
    {
        std::map< std::string, std::string >::iterator iter;
        for (iter = m_key_default.begin(); iter != m_key_default.end(); ++iter)
        {
            if (m_keys[iter->first] == graph_key)
                handle_graph_property(iter->first, iter->second);
        }
    }

    void handle_graph_property(
        const std::string& key_id, const std::string& value)
    {
        m_g.set_graph_property(m_key_name[key_id], value, m_key_type[key_id]);
    }

    void handle_node_property(const std::string& key_id,
        const std::string& descriptor, const std::string& value)
    {
        m_g.set_vertex_property(m_key_name[key_id], m_vertex[descriptor], value,
            m_key_type[key_id]);
    }

    void handle_edge_property(
        const std::string& key_id, size_t descriptor, const std::string& value)
    {
        m_g.set_edge_property(
            m_key_name[key_id], m_edge[descriptor], value, m_key_type[key_id]);
    }

    mutate_graph& m_g;
    std::map< std::string, key_kind > m_keys;
    std::map< std::string, std::string > m_key_name;
    std::map< std::string, std::string > m_key_type;
    std::map< std::string, std::string > m_key_default;
    std::map< std::string, any > m_vertex;
    std::vector< any > m_edge;
};

}

namespace boost
{
void BOOST_GRAPH_DECL read_graphml(
    std::istream& in, mutate_graph& g, size_t desired_idx)
{
    graphml_reader reader(g);
    reader.run(in, desired_idx);
}
}
