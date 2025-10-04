// Copyright 2004-9 Trustees of Indiana University

// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//
// read_graphviz_new.cpp -
//   Initialize a model of the BGL's MutableGraph concept and an associated
//  collection of property maps using a graph expressed in the GraphViz
// DOT Language.
//
//   Based on the grammar found at:
//   https://web.archive.org/web/20041213234742/http://www.graphviz.org/cvs/doc/info/lang.html
//
//   Jeremiah rewrite used grammar found at:
//   http://www.graphviz.org/doc/info/lang.html
//   and page 34 or http://www.graphviz.org/pdf/dotguide.pdf
//
//   See documentation for this code at:
//     http://www.boost.org/libs/graph/doc/read_graphviz.html
//

// Author: Jeremiah Willcock
//         Ronald Garcia
//

#define BOOST_GRAPH_SOURCE
#include <boost/assert.hpp>
#include <boost/ref.hpp>
#include <boost/function/function2.hpp>
#include <boost/property_map/dynamic_property_map.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <cstdlib>
#include <algorithm>
#include <exception> // for std::exception
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>
#include <boost/throw_exception.hpp>
#include <boost/regex.hpp>
#include <boost/function.hpp>
#include <boost/graph/dll_import_export.hpp>
#include <boost/graph/graphviz.hpp>

namespace boost
{

namespace read_graphviz_detail
{
    static const long max_subgraph_nesting_level = 255;
    struct token
    {
        enum token_type
        {
            kw_strict,
            kw_graph,
            kw_digraph,
            kw_node,
            kw_edge,
            kw_subgraph,
            left_brace,
            right_brace,
            semicolon,
            equal,
            left_bracket,
            right_bracket,
            comma,
            colon,
            dash_greater,
            dash_dash,
            plus,
            left_paren,
            right_paren,
            at,
            identifier,
            quoted_string, // Only used internally in tokenizer
            eof,
            invalid
        };
        token_type type;
        std::string normalized_value; // May have double-quotes removed and/or
                                      // some escapes replaced
        token(token_type type, const std::string& normalized_value)
        : type(type), normalized_value(normalized_value)
        {
        }
        token() : type(invalid), normalized_value("") {}
        friend std::ostream& operator<<(std::ostream& o, const token& t)
        {
            switch (t.type)
            {
            case token::kw_strict:
                o << "<strict>";
                break;
            case token::kw_graph:
                o << "<graph>";
                break;
            case token::kw_digraph:
                o << "<digraph>";
                break;
            case token::kw_node:
                o << "<node>";
                break;
            case token::kw_edge:
                o << "<edge>";
                break;
            case token::kw_subgraph:
                o << "<subgraph>";
                break;
            case token::left_brace:
                o << "<left_brace>";
                break;
            case token::right_brace:
                o << "<right_brace>";
                break;
            case token::semicolon:
                o << "<semicolon>";
                break;
            case token::equal:
                o << "<equal>";
                break;
            case token::left_bracket:
                o << "<left_bracket>";
                break;
            case token::right_bracket:
                o << "<right_bracket>";
                break;
            case token::comma:
                o << "<comma>";
                break;
            case token::colon:
                o << "<colon>";
                break;
            case token::dash_greater:
                o << "<dash-greater>";
                break;
            case token::dash_dash:
                o << "<dash-dash>";
                break;
            case token::plus:
                o << "<plus>";
                break;
            case token::left_paren:
                o << "<left_paren>";
                break;
            case token::right_paren:
                o << "<right_paren>";
                break;
            case token::at:
                o << "<at>";
                break;
            case token::identifier:
                o << "<identifier>";
                break;
            case token::quoted_string:
                o << "<quoted_string>";
                break;
            case token::eof:
                o << "<eof>";
                break;
            default:
                o << "<invalid type>";
                break;
            }
            o << " '" << t.normalized_value << "'";
            return o;
        }
    };

    bad_graphviz_syntax lex_error(const std::string& errmsg, char bad_char)
    {
        if (bad_char == '\0')
        {
            return bad_graphviz_syntax(errmsg + " (at end of input)");
        }
        else
        {
            return bad_graphviz_syntax(
                errmsg + " (char is '" + bad_char + "')");
        }
    }

    bad_graphviz_syntax parse_error(
        const std::string& errmsg, const token& bad_token)
    {
        return bad_graphviz_syntax(errmsg + " (token is \""
            + boost::lexical_cast< std::string >(bad_token) + "\")");
    }

    struct tokenizer
    {
        std::string::const_iterator begin, end;
        std::vector< token > lookahead;
        // Precomputed regexes
        boost::regex stuff_to_skip;
        boost::regex basic_id_token;
        boost::regex punctuation_token;
        boost::regex number_token;
        boost::regex quoted_string_token;
        boost::regex xml_tag_token;
        boost::regex cdata;

        tokenizer(const std::string& str) : begin(str.begin()), end(str.end())
        {
            // std::string end_of_token = "(?=(?:\\W))"; // SEHE: unused?
            std::string whitespace = "(?:\\s+)";
            std::string slash_slash_comment = "(?://.*?$)";
            std::string slash_star_comment = "(?:/\\*.*?\\*/)";
            std::string hash_comment = "(?:^#.*?$)";
            std::string backslash_newline = "(?:[\\\\][\\n])";
            stuff_to_skip = "\\A(?:" + whitespace + "|" + slash_slash_comment
                + "|" + slash_star_comment + "|" + hash_comment + "|"
                + backslash_newline + ")*";
            basic_id_token = "\\A([[:alpha:]_](?:\\w*))";
            punctuation_token = "\\A([][{};=,:+()@]|[-][>-])";
            number_token = "\\A([-]?(?:(?:\\.\\d+)|(?:\\d+(?:\\.\\d*)?)))";
            quoted_string_token = "\\A(\"(?:[^\"\\\\]|(?:[\\\\].))*\")";
            xml_tag_token
                = "\\A<(/?)(?:[^!?'\"]|(?:'[^']*?')|(?:\"[^\"]*?\"))*?(/?)>";
            cdata = "\\A\\Q<![CDATA[\\E.*?\\Q]]>\\E";
        }

        void skip()
        {
            boost::match_results< std::string::const_iterator > results;
#ifndef NDEBUG
            bool found =
#endif
                boost::regex_search(begin, end, results, stuff_to_skip);
#ifndef NDEBUG
            BOOST_ASSERT(found);
#endif
            boost::sub_match< std::string::const_iterator > sm1
                = results.suffix();
            BOOST_ASSERT(sm1.second == end);
            begin = sm1.first;
        }

        token get_token_raw()
        {
            if (!lookahead.empty())
            {
                token t = lookahead.front();
                lookahead.erase(lookahead.begin());
                return t;
            }
            skip();
            if (begin == end)
                return token(token::eof, "");
            // Look for keywords first
            bool found;
            boost::match_results< std::string::const_iterator > results;
            found = boost::regex_search(begin, end, results, basic_id_token);
            if (found)
            {
                std::string str = results[1].str();
                std::string str_lower = boost::algorithm::to_lower_copy(str);
                begin = results.suffix().first;
                if (str_lower == "strict")
                {
                    return token(token::kw_strict, str);
                }
                else if (str_lower == "graph")
                {
                    return token(token::kw_graph, str);
                }
                else if (str_lower == "digraph")
                {
                    return token(token::kw_digraph, str);
                }
                else if (str_lower == "node")
                {
                    return token(token::kw_node, str);
                }
                else if (str_lower == "edge")
                {
                    return token(token::kw_edge, str);
                }
                else if (str_lower == "subgraph")
                {
                    return token(token::kw_subgraph, str);
                }
                else
                {
                    return token(token::identifier, str);
                }
            }
            found = boost::regex_search(begin, end, results, punctuation_token);
            if (found)
            {
                std::string str = results[1].str();
                begin = results.suffix().first;
                switch (str[0])
                {
                case '[':
                    return token(token::left_bracket, str);
                case ']':
                    return token(token::right_bracket, str);
                case '{':
                    return token(token::left_brace, str);
                case '}':
                    return token(token::right_brace, str);
                case ';':
                    return token(token::semicolon, str);
                case '=':
                    return token(token::equal, str);
                case ',':
                    return token(token::comma, str);
                case ':':
                    return token(token::colon, str);
                case '+':
                    return token(token::plus, str);
                case '(':
                    return token(token::left_paren, str);
                case ')':
                    return token(token::right_paren, str);
                case '@':
                    return token(token::at, str);
                case '-':
                {
                    switch (str[1])
                    {
                    case '-':
                        return token(token::dash_dash, str);
                    case '>':
                        return token(token::dash_greater, str);
                    default:
                        BOOST_ASSERT(!"Definition of punctuation_token does "
                                      "not match switch statement");
                    }
                    // Prevent static analyzers complaining about fallthrough:
                    break;
                }
                default:
                    BOOST_ASSERT(!"Definition of punctuation_token does not "
                                  "match switch statement");
                }
            }
            found = boost::regex_search(begin, end, results, number_token);
            if (found)
            {
                std::string str = results[1].str();
                begin = results.suffix().first;
                return token(token::identifier, str);
            }
            found
                = boost::regex_search(begin, end, results, quoted_string_token);
            if (found)
            {
                std::string str = results[1].str();
                begin = results.suffix().first;
                // Remove the beginning and ending quotes
                BOOST_ASSERT(str.size() >= 2);
                str.erase(str.begin());
                str.erase(str.end() - 1);
                // Unescape quotes in the middle, but nothing else (see format
                // spec)
                for (size_t i = 0; i + 1 < str.size() /* May change */; ++i)
                {
                    if (str[i] == '\\' && str[i + 1] == '"')
                    {
                        str.erase(str.begin() + i);
                        // Don't need to adjust i
                    }
                    else if (str[i] == '\\' && str[i + 1] == '\n')
                    {
                        str.erase(str.begin() + i);
                        str.erase(str.begin() + i);
                        --i; // Invert ++ that will be applied
                    }
                }
                return token(token::quoted_string, str);
            }
            if (*begin == '<')
            {
                std::string::const_iterator saved_begin = begin;
                int counter = 0;
                do
                {
                    if (begin == end)
                        throw_lex_error("Unclosed HTML string");
                    if (*begin != '<')
                    {
                        ++begin;
                        continue;
                    }
                    found = boost::regex_search(
                        begin, end, results, xml_tag_token);
                    if (found)
                    {
                        begin = results.suffix().first;
                        if (results[1].str() == "/")
                        { // Close tag
                            --counter;
                        }
                        else if (results[2].str() == "/")
                        { // Empty tag
                        }
                        else
                        { // Open tag
                            ++counter;
                        }
                        continue;
                    }
                    found = boost::regex_search(begin, end, results, cdata);
                    if (found)
                    {
                        begin = results.suffix().first;
                        continue;
                    }
                    throw_lex_error("Invalid contents in HTML string");
                } while (counter > 0);
                return token(
                    token::identifier, std::string(saved_begin, begin));
            }
            else
            {
                throw_lex_error("Invalid character");
                return token();
            }
        }

        token peek_token_raw()
        {
            if (lookahead.empty())
            {
                token t = get_token_raw();
                lookahead.push_back(t);
            }
            return lookahead.front();
        }

        token get_token()
        { // Handle string concatenation
            token t = get_token_raw();
            if (t.type != token::quoted_string)
                return t;
            std::string str = t.normalized_value;
            while (peek_token_raw().type == token::plus)
            {
                get_token_raw();
                token t2 = get_token_raw();
                if (t2.type != token::quoted_string)
                {
                    throw_lex_error(
                        "Must have quoted string after string concatenation");
                }
                str += t2.normalized_value;
            }
            return token(
                token::identifier, str); // Note that quoted_string does not get
                                         // passed to the parser
        }

        void throw_lex_error(const std::string& errmsg)
        {
            boost::throw_exception(
                lex_error(errmsg, (begin == end ? '\0' : *begin)));
        }
    };

    struct edge_endpoint
    {
        bool is_subgraph;
        node_and_port node_ep;
        subgraph_name subgraph_ep;

        static edge_endpoint node(const node_and_port& ep)
        {
            edge_endpoint r;
            r.is_subgraph = false;
            r.node_ep = ep;
            return r;
        }

        static edge_endpoint subgraph(const subgraph_name& ep)
        {
            edge_endpoint r;
            r.is_subgraph = true;
            r.subgraph_ep = ep;
            return r;
        }
    };

    struct node_or_subgraph_ref
    {
        bool is_subgraph;
        std::string
            name; // Name for subgraphs or nodes, "___root___" for root graph
    };

    static node_or_subgraph_ref noderef(const node_name& n)
    {
        node_or_subgraph_ref r;
        r.is_subgraph = false;
        r.name = n;
        return r;
    }

    static node_or_subgraph_ref subgraphref(const subgraph_name& n)
    {
        node_or_subgraph_ref r;
        r.is_subgraph = true;
        r.name = n;
        return r;
    }

    typedef std::vector< node_or_subgraph_ref > subgraph_member_list;

    struct subgraph_info
    {
        properties def_node_props;
        properties def_edge_props;
        subgraph_member_list members;
    };

    struct parser
    {
        tokenizer the_tokenizer;
        std::vector< token > lookahead;
        parser_result& r;
        std::map< subgraph_name, subgraph_info > subgraphs;
        std::string current_subgraph_name;
        int sgcounter; // Counter for anonymous subgraphs
        long sgnesting_level;
        std::set< std::pair< node_name, node_name > >
            existing_edges; // Used for checking in strict graphs

        subgraph_info& current() { return subgraphs[current_subgraph_name]; }
        properties& current_graph_props()
        {
            return r.graph_props[current_subgraph_name];
        }
        subgraph_member_list& current_members() { return current().members; }

        parser(const std::string& gr, parser_result& result)
        : the_tokenizer(gr), lookahead(), r(result), sgcounter(0), sgnesting_level(0)
        {
            current_subgraph_name = "___root___";
            current() = subgraph_info(); // Initialize root graph
            current_graph_props().clear();
            current_members().clear();
        }

        token get()
        {
            if (lookahead.empty())
            {
                token t = the_tokenizer.get_token();
                return t;
            }
            else
            {
                token t = lookahead.front();
                lookahead.erase(lookahead.begin());
                return t;
            }
        }

        token peek()
        {
            if (lookahead.empty())
            {
                lookahead.push_back(the_tokenizer.get_token());
            }
            return lookahead.front();
        }

        void error(const std::string& str)
        {
            boost::throw_exception(parse_error(str, peek()));
        }

        void parse_graph(bool want_directed)
        {
            bool is_strict = false;
            bool is_directed = false;
            std::string name;
            if (peek().type == token::kw_strict)
            {
                get();
                is_strict = true;
            }
            switch (peek().type)
            {
            case token::kw_graph:
                is_directed = false;
                break;
            case token::kw_digraph:
                is_directed = true;
                break;
            default:
                error("Wanted \"graph\" or \"digraph\"");
            }
            r.graph_is_directed = is_directed; // Used to check edges
            r.graph_is_strict = is_strict;
            if (want_directed != r.graph_is_directed)
            {
                if (want_directed)
                {
                    boost::throw_exception(boost::undirected_graph_error());
                }
                else
                {
                    boost::throw_exception(boost::directed_graph_error());
                }
            }
            get();
            switch (peek().type)
            {
            case token::identifier:
                name = peek().normalized_value;
                get();
                break;
            case token::left_brace:
                break;
            default:
                error("Wanted a graph name or left brace");
            }
            if (peek().type == token::left_brace)
                get();
            else
                error("Wanted a left brace to start the graph");
            parse_stmt_list();
            if (peek().type == token::right_brace)
                get();
            else
                error("Wanted a right brace to end the graph");
            if (peek().type == token::eof)
            {
            }
            else
                error("Wanted end of file");
        }

        void parse_stmt_list()
        {
            while (true)
            {
                if (peek().type == token::right_brace)
                    return;
                parse_stmt();
                if (peek().type == token::semicolon)
                    get();
            }
        }

        void parse_stmt()
        {
            switch (peek().type)
            {
            case token::kw_node:
            case token::kw_edge:
            case token::kw_graph:
                parse_attr_stmt();
                break;
            case token::kw_subgraph:
            case token::left_brace:
            case token::identifier:
            {
                token id = get();
                if (id.type == token::identifier && peek().type == token::equal)
                { // Graph property
                    get();
                    if (peek().type != token::identifier)
                        error("Wanted identifier as right side of =");
                    token id2 = get();
                    current_graph_props()[id.normalized_value]
                        = id2.normalized_value;
                }
                else
                {
                    edge_endpoint ep = parse_endpoint_rest(id);
                    if (peek().type == token::dash_dash
                        || peek().type == token::dash_greater)
                    { // Edge
                        parse_edge_stmt(ep);
                    }
                    else
                    {
                        if (!ep.is_subgraph)
                        { // Only nodes can have attribute lists
                            // This node already exists because of its first
                            // mention (properties set to defaults by
                            // parse_node_and_port, called by
                            // parse_endpoint_rest)
                            properties this_node_props;
                            if (peek().type == token::left_bracket)
                            {
                                parse_attr_list(this_node_props);
                            }
                            for (properties::const_iterator i
                                 = this_node_props.begin();
                                 i != this_node_props.end(); ++i)
                            {
                                // Override old properties with same names
                                r.nodes[ep.node_ep.name][i->first] = i->second;
                            }
                            current_members().push_back(
                                noderef(ep.node_ep.name));
                        }
                        else
                        {
                            current_members().push_back(
                                subgraphref(ep.subgraph_ep));
                        }
                    }
                }
                break;
            }
            default:
                error("Invalid start token for statement");
            }
        }

        void parse_attr_stmt()
        {
            switch (get().type)
            {
            case token::kw_graph:
                parse_attr_list(current_graph_props());
                break;
            case token::kw_node:
                parse_attr_list(current().def_node_props);
                break;
            case token::kw_edge:
                parse_attr_list(current().def_edge_props);
                break;
            default:
                BOOST_ASSERT(!"Bad attr_stmt case");
            }
        }

        edge_endpoint parse_endpoint()
        {
            switch (peek().type)
            {
            case token::kw_subgraph:
            case token::left_brace:
            case token::identifier:
            {
                token first = get();
                return parse_endpoint_rest(first);
            }
            default:
            {
                error("Wanted \"subgraph\", \"{\", or identifier to start node "
                      "or subgraph");
                return edge_endpoint();
            }
            }
        }

        edge_endpoint parse_endpoint_rest(const token& first_token)
        {
            switch (first_token.type)
            {
            case token::kw_subgraph:
            case token::left_brace:
                return edge_endpoint::subgraph(parse_subgraph(first_token));
            default:
                return edge_endpoint::node(parse_node_and_port(first_token));
            }
        }

        subgraph_name parse_subgraph(const token& first_token)
        {
            std::string name;
            bool is_anonymous = true;
            if (first_token.type == token::kw_subgraph)
            {
                switch (peek().type)
                {
                case token::identifier:
                    name = get().normalized_value;
                    is_anonymous = false;
                    break;
                case token::left_brace:
                    is_anonymous = true;
                    break;
                default:
                    error("Subgraph reference needs a name");
                    break;
                }
            }
            if (is_anonymous)
            {
                name = "___subgraph_"
                    + boost::lexical_cast< std::string >(++sgcounter);
            }
            if (subgraphs.find(name) == subgraphs.end())
            {
                subgraphs[name]
                    = current(); // Initialize properties and defaults
                subgraphs[name].members.clear(); // Except member list
            }
            if (!is_anonymous && peek().type != token::left_brace)
            {
                return name;
            }
            subgraph_name old_sg = current_subgraph_name;
            if (++sgnesting_level > max_subgraph_nesting_level)
            {
                error("Exceeded maximum subgraph nesting level");
            }
            current_subgraph_name = name;
            if (first_token.type != token::left_brace)
            {
                if (peek().type == token::left_brace)
                    get();
                else
                    error("Wanted left brace to start subgraph");
            }
            parse_stmt_list();
            if (peek().type == token::right_brace)
                get();
            else
                error("Wanted right brace to end subgraph");
            current_subgraph_name = old_sg;
            sgnesting_level -= 1;
            return name;
        }

        node_and_port parse_node_and_port(const token& name)
        {
            // A node ID is a node name, followed optionally by a port angle and
            // a port location (in either order); a port location is either :id,
            // :id:id, or :(id,id); the last two forms are treated as equivalent
            // although I am not sure about that.
            node_and_port id;
            id.name = name.normalized_value;
        parse_more:
            switch (peek().type)
            {
            case token::at:
            {
                get();
                if (peek().type != token::identifier)
                    error("Wanted identifier as port angle");
                if (!id.angle.empty())
                    error("Duplicate port angle");
                id.angle = get().normalized_value;
                goto parse_more;
            }
            case token::colon:
            {
                get();
                if (!id.location.empty())
                    error("Duplicate port location");
                switch (peek().type)
                {
                case token::identifier:
                {
                    id.location.push_back(get().normalized_value);
                    switch (peek().type)
                    {
                    case token::colon:
                    {
                        get();
                        if (peek().type != token::identifier)
                            error("Wanted identifier as port location");
                        id.location.push_back(get().normalized_value);
                        goto parse_more;
                    }
                    default:
                        goto parse_more;
                    }
                }
                case token::left_paren:
                {
                    get();
                    if (peek().type != token::identifier)
                        error("Wanted identifier as first element of port "
                              "location");
                    id.location.push_back(get().normalized_value);
                    if (peek().type != token::comma)
                        error("Wanted comma between parts of port location");
                    get();
                    if (peek().type != token::identifier)
                        error("Wanted identifier as second element of port "
                              "location");
                    id.location.push_back(get().normalized_value);
                    if (peek().type != token::right_paren)
                        error(
                            "Wanted right parenthesis to close port location");
                    get();
                    goto parse_more;
                }
                default:
                    error("Wanted identifier or left parenthesis as start of "
                          "port location");
                }
            }
            break;
            default:
                break;
            }
            if (r.nodes.find(id.name) == r.nodes.end())
            { // First mention
                r.nodes[id.name] = current().def_node_props;
            }
            return id;
        }

        void parse_edge_stmt(const edge_endpoint& lhs)
        {
            std::vector< edge_endpoint > nodes_in_chain(1, lhs);
            while (true)
            {
                bool leave_loop = true;
                switch (peek().type)
                {
                case token::dash_dash:
                {
                    if (r.graph_is_directed)
                        error("Using -- in directed graph");
                    get();
                    nodes_in_chain.push_back(parse_endpoint());
                    leave_loop = false;
                    break;
                }
                case token::dash_greater:
                {
                    if (!r.graph_is_directed)
                        error("Using -> in undirected graph");
                    get();
                    nodes_in_chain.push_back(parse_endpoint());
                    leave_loop = false;
                    break;
                }
                default:
                    leave_loop = true;
                    break;
                }
                if (leave_loop)
                    break;
            }
            properties this_edge_props = current().def_edge_props;
            if (peek().type == token::left_bracket)
                parse_attr_list(this_edge_props);
            BOOST_ASSERT(nodes_in_chain.size()
                >= 2); // Should be in node parser otherwise
            for (size_t i = 0; i + 1 < nodes_in_chain.size(); ++i)
            {
                do_orig_edge(
                    nodes_in_chain[i], nodes_in_chain[i + 1], this_edge_props);
            }
        }

        // Do an edge from the file, the edge may need to be expanded if it
        // connects to a subgraph
        void do_orig_edge(const edge_endpoint& src, const edge_endpoint& tgt,
            const properties& props)
        {
            std::set< node_and_port > sources = get_recursive_members(src);
            std::set< node_and_port > targets = get_recursive_members(tgt);
            for (std::set< node_and_port >::const_iterator i = sources.begin();
                 i != sources.end(); ++i)
            {
                for (std::set< node_and_port >::const_iterator j
                     = targets.begin();
                     j != targets.end(); ++j)
                {
                    do_edge(*i, *j, props);
                }
            }
        }

        // Get nodes in an edge_endpoint, recursively
        std::set< node_and_port > get_recursive_members(
            const edge_endpoint& orig_ep)
        {
            std::set< node_and_port > result;
            std::vector< edge_endpoint > worklist(1, orig_ep);
            std::set< subgraph_name > done;
            while (!worklist.empty())
            {
                edge_endpoint ep = worklist.back();
                worklist.pop_back();
                if (ep.is_subgraph)
                {
                    if (done.find(ep.subgraph_ep) == done.end())
                    {
                        done.insert(ep.subgraph_ep);
                        std::map< subgraph_name, subgraph_info >::const_iterator
                            info_i
                            = subgraphs.find(ep.subgraph_ep);
                        if (info_i != subgraphs.end())
                        {
                            const subgraph_member_list& members
                                = info_i->second.members;
                            for (subgraph_member_list::const_iterator i
                                 = members.begin();
                                 i != members.end(); ++i)
                            {
                                node_or_subgraph_ref ref = *i;
                                if (ref.is_subgraph)
                                {
                                    worklist.push_back(
                                        edge_endpoint::subgraph(ref.name));
                                }
                                else
                                {
                                    node_and_port np;
                                    np.name = ref.name;
                                    worklist.push_back(edge_endpoint::node(np));
                                }
                            }
                        }
                    }
                }
                else
                {
                    result.insert(ep.node_ep);
                }
            }
            return result;
        }

        // Do a fixed-up edge, with only nodes as endpoints
        void do_edge(const node_and_port& src, const node_and_port& tgt,
            const properties& props)
        {
            if (r.graph_is_strict)
            {
                if (src.name == tgt.name)
                    return;
                std::pair< node_name, node_name > tag(src.name, tgt.name);
                if (existing_edges.find(tag) != existing_edges.end())
                {
                    return; // Parallel edge
                }
                existing_edges.insert(tag);
            }
            edge_info e;
            e.source = src;
            e.target = tgt;
            e.props = props;
            r.edges.push_back(e);
        }

        void parse_attr_list(properties& props)
        {
            while (true)
            {
                if (peek().type == token::left_bracket)
                    get();
                else
                    error("Wanted left bracket to start attribute list");
                while (true)
                {
                    switch (peek().type)
                    {
                    case token::right_bracket:
                        break;
                    case token::identifier:
                    {
                        std::string lhs = get().normalized_value;
                        std::string rhs = "true";
                        if (peek().type == token::equal)
                        {
                            get();
                            if (peek().type != token::identifier)
                                error(
                                    "Wanted identifier as value of attribute");
                            rhs = get().normalized_value;
                        }
                        props[lhs] = rhs;
                        break;
                    }
                    default:
                        error("Wanted identifier as name of attribute");
                    }
                    if (peek().type == token::comma
                        || peek().type == token::semicolon)
                        get();
                    else if (peek().type == token::right_bracket)
                        break;
                }
                if (peek().type == token::right_bracket)
                    get();
                else
                    error("Wanted right bracket to end attribute list");
                if (peek().type != token::left_bracket)
                    break;
            }
        }
    };

    void parse_graphviz_from_string(
        const std::string& str, parser_result& result, bool want_directed)
    {
        parser p(str, result);
        p.parse_graph(want_directed);
    }

    // Some debugging stuff
    std::ostream& operator<<(std::ostream& o, const node_and_port& n)
    {
        o << n.name;
        for (size_t i = 0; i < n.location.size(); ++i)
        {
            o << ":" << n.location[i];
        }
        if (!n.angle.empty())
            o << "@" << n.angle;
        return o;
    }

    // Can't be operator<< because properties is just an std::map
    std::string props_to_string(const properties& props)
    {
        std::string result = "[";
        for (properties::const_iterator i = props.begin(); i != props.end();
             ++i)
        {
            if (i != props.begin())
                result += ", ";
            result += i->first + "=" + i->second;
        }
        result += "]";
        return result;
    }

    void translate_results_to_graph(
        const parser_result& r, ::boost::detail::graph::mutate_graph* mg)
    {
        typedef boost::detail::graph::edge_t edge;
        for (std::map< node_name, properties >::const_iterator i
             = r.nodes.begin();
             i != r.nodes.end(); ++i)
        {
            // std::cerr << i->first << " " << props_to_string(i->second) <<
            // std::endl;
            mg->do_add_vertex(i->first);
            for (properties::const_iterator j = i->second.begin();
                 j != i->second.end(); ++j)
            {
                mg->set_node_property(j->first, i->first, j->second);
            }
        }
        for (std::vector< edge_info >::const_iterator i = r.edges.begin();
             i != r.edges.end(); ++i)
        {
            const edge_info& ei = *i;
            // std::cerr << ei.source << " -> " << ei.target << " " <<
            // props_to_string(ei.props) << std::endl;
            edge e = edge::new_edge();
            mg->do_add_edge(e, ei.source.name, ei.target.name);
            for (properties::const_iterator j = ei.props.begin();
                 j != ei.props.end(); ++j)
            {
                mg->set_edge_property(j->first, e, j->second);
            }
        }
        std::map< subgraph_name, properties >::const_iterator root_graph_props_i
            = r.graph_props.find("___root___");
        BOOST_ASSERT(
            root_graph_props_i != r.graph_props.end()); // Should not happen
        const properties& root_graph_props = root_graph_props_i->second;
        // std::cerr << "ending graph " << props_to_string(root_graph_props) <<
        // std::endl;
        for (properties::const_iterator i = root_graph_props.begin();
             i != root_graph_props.end(); ++i)
        {
            mg->set_graph_property(i->first, i->second);
        }
        mg->finish_building_graph();
    }

} // end namespace read_graphviz_detail

namespace detail
{
    namespace graph
    {

        BOOST_GRAPH_DECL bool read_graphviz_new(
            const std::string& str, boost::detail::graph::mutate_graph* mg)
        {
            read_graphviz_detail::parser_result parsed_file;
            read_graphviz_detail::parse_graphviz_from_string(
                str, parsed_file, mg->is_directed());
            read_graphviz_detail::translate_results_to_graph(parsed_file, mg);
            return true;
        }

    } // end namespace graph
} // end namespace detail

} // end namespace boost

// GraphViz format notes (tested using "dot version 1.13 (v16) (Mon August 23,
// 2004)", grammar from references in read_graphviz_new.hpp):

// Subgraphs don't nest (all a0 subgraphs are the same thing), but a node or
// subgraph can have multiple parents (sources online say that the layout
// algorithms can't handle non-tree structures of clusters, but it seems to
// read them the same from the file).  The containment relation is required to
// be a DAG, though; it appears that a node or subgraph can't be moved into an
// ancestor of a subgraph where it already was (we don't enforce that but do a
// DFS when finding members to prevent cycles).  Nodes get their properties by
// when they are first mentioned, and can only have them overridden after that
// by explicit properties on that particular node.  Closing and reopening the
// same subgraph name adds to its members, and graph properties and node/edge
// defaults are preserved in that subgraph.  The members of a subgraph used in
// an edge are gathered when the edge is read, even if new members are added to
// the subgraph later.  Ports are allowed in a lot more places in the grammar
// than Dot uses.  For example, declaring a node seems to ignore ports, and I
// don't think it's possible to set properties on a particular port.  Adding an
// edge between two ports on the same node seems to make Dot unhappy (crashed
// for me).

// Test graph for GraphViz behavior on strange combinations of subgraphs and
// such.  I don't have anywhere else to put this file.

#if 0
dIGRaph foo {
  node [color=blue]
  subgraph a -> b
  subgraph a {c}
  subgraph a -> d
  subgraph a {node [color=red]; e}
  subgraph a -> f
  subgraph a {g} -> h
  subgraph a {node [color=green]; i} -> j
  subgraph a {node [color=yellow]}

  subgraph a0 {node [color=black]; subgraph a1 {node [color=white]}}
  node [color=pink] zz
  subgraph a0 {x1}
  subgraph a0 {subgraph a1 {x2}}

  subgraph a0 -> x3
  subgraph a0 {subgraph a1 -> x3}
  x3
  subgraph a0 {subgraph a0 {node [color=xxx]; x2} x7}
  x2 [color=yyy]
  subgraph cluster_ax {foo; subgraph a0}
  subgraph a0 {foo2}
  subgraph cluster_ax {foo3}
  // subgraph a0 -> subgraph a0

  bgcolor=yellow
  subgraph cluster_a2 {y1}
  // y1:n -> y1:(s,3)@se
  y1@se [color=green]
  y1@n [color=red]
}
#endif
