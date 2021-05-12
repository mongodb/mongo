/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   filter_parser.cpp
 * \author Andrey Semashev
 * \date   31.03.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WITHOUT_SETTINGS_PARSERS

#include <boost/log/detail/setup_config.hpp>
#include <cstddef>
#include <map>
#include <stack>
#include <string>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <boost/assert.hpp>
#include <boost/none.hpp>
#include <boost/move/core.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>
#include <boost/phoenix/core.hpp>
#include <boost/phoenix/bind/bind_function_object.hpp>
#include <boost/phoenix/operator/logical.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/utility/setup/filter_parser.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/log/detail/locks.hpp>
#include <boost/log/detail/light_rw_mutex.hpp>
#endif // !defined(BOOST_LOG_NO_THREADS)
#include "parser_utils.hpp"
#if !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)
#include "default_filter_factory.hpp"
#endif
#include "spirit_encoding.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! Filter factories repository
template< typename CharT >
struct filters_repository :
    public log::aux::lazy_singleton< filters_repository< CharT > >
{
    typedef CharT char_type;
    typedef log::aux::lazy_singleton< filters_repository< char_type > > base_type;
    typedef std::basic_string< char_type > string_type;
    typedef filter_factory< char_type > filter_factory_type;

    //! Attribute name ordering predicate
    struct attribute_name_order
    {
        typedef bool result_type;
        result_type operator() (attribute_name const& left, attribute_name const& right) const
        {
            return left.id() < right.id();
        }
    };

    typedef std::map< attribute_name, shared_ptr< filter_factory_type >, attribute_name_order > factories_map;

#if !defined(BOOST_LOG_BROKEN_FRIEND_TEMPLATE_SPECIALIZATIONS)
    friend class log::aux::lazy_singleton< filters_repository< char_type > >;
#else
    friend class base_type;
#endif

#if !defined(BOOST_LOG_NO_THREADS)
    //! Synchronization mutex
    mutable log::aux::light_rw_mutex m_Mutex;
#endif
    //! The map of filter factories
    factories_map m_Map;
#if !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)
    //! Default factory
    mutable aux::default_filter_factory< char_type > m_DefaultFactory;
#endif

    //! The method returns the filter factory for the specified attribute name
    filter_factory_type& get_factory(attribute_name const& name) const
    {
        typename factories_map::const_iterator it = m_Map.find(name);
        if (it != m_Map.end())
        {
            return *it->second;
        }
        else
        {
#if !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)
            return m_DefaultFactory;
#else
            BOOST_LOG_THROW_DESCR(setup_error, "No filter factory registered for attribute " + name.string());
#endif
        }
    }

private:
    filters_repository()
    {
    }
};

//! Filter parser
template< typename CharT >
class filter_parser
{
private:
    typedef CharT char_type;
    typedef const char_type* iterator_type;
    typedef typename log::aux::encoding< char_type >::type encoding;
    typedef log::aux::encoding_specific< encoding > encoding_specific;
    typedef std::basic_string< char_type > string_type;
    typedef log::aux::char_constants< char_type > constants;
    typedef filter_factory< char_type > filter_factory_type;

    typedef filter (filter_factory_type::*comparison_relation_handler_t)(attribute_name const&, string_type const&);

private:
    //! Parsed attribute name
    mutable attribute_name m_AttributeName;
    //! The second operand of a relation
    mutable optional< string_type > m_Operand;
    //! Comparison relation handler
    comparison_relation_handler_t m_ComparisonRelation;
    //! The custom relation string
    mutable string_type m_CustomRelation;

    //! Filter subexpressions as they are parsed
    mutable std::stack< filter > m_Subexpressions;

public:
    //! Constructor
    filter_parser() :
        m_ComparisonRelation(NULL)
    {
    }

    //! The method returns the constructed filter
    filter get_filter()
    {
        if (m_Subexpressions.empty())
            return filter();
        return boost::move(m_Subexpressions.top());
    }

    //! The pethod parses filter from the string
    void parse(iterator_type& begin, iterator_type end, unsigned int depth = 0)
    {
        typedef void (filter_parser::*logical_op_t)();
        logical_op_t logical_op = NULL;
        iterator_type p = constants::trim_spaces_left(begin, end);
        while (p != end)
        {
            // Parse subexpression
            parse_subexpression(p, end, depth);
            if (logical_op)
            {
                // This was the right-hand subexpression. Compose the two top subexpressions into a single filter.
                (this->*logical_op)();
                logical_op = NULL;
            }

            p = constants::trim_spaces_left(p, end);
            if (p != end)
            {
                char_type c = *p;
                iterator_type next = p + 1;
                if (c == constants::char_paren_bracket_right)
                {
                    // The subexpression has ended
                    if (depth == 0)
                        BOOST_LOG_THROW_DESCR(parse_error, "Invalid filter definition: unmatched closing parenthesis");

                    p = next;
                    --depth;
                    break;
                }
                else if (c == constants::char_and || scan_keyword(p, end, next, constants::and_keyword()))
                {
                    logical_op = &filter_parser::on_and;
                }
                else if (c == constants::char_or || scan_keyword(p, end, next, constants::or_keyword()))
                {
                    logical_op = &filter_parser::on_or;
                }
                else
                {
                    BOOST_LOG_THROW_DESCR(parse_error, "Invalid filter definition: unexpected character encountered");
                }

                p = constants::trim_spaces_left(next, end);
            }
            else
                break;
        }

        if (logical_op)
        {
            BOOST_LOG_THROW_DESCR(parse_error, "Invalid filter definition: logical operation without the right-hand subexpression");
        }

        if (p == end && depth > 0)
        {
            BOOST_LOG_THROW_DESCR(parse_error, "Invalid filter definition: unterminated parenthesis");
        }

        begin = p;
    }

private:
    //! The method parses a single subexpression
    void parse_subexpression(iterator_type& begin, iterator_type end, unsigned int depth)
    {
        bool negated = false, negation_present = false, done = false;
        iterator_type p = begin;

        while (p != end)
        {
            char_type c = *p;
            iterator_type next = p + 1;
            if (c == constants::char_percent)
            {
                // We found an attribute placeholder
                iterator_type start = constants::trim_spaces_left(next, end);
                p = constants::scan_attr_placeholder(start, end);
                if (p == end)
                    BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder in the filter string");

                on_attribute_name(start, p);

                p = constants::trim_spaces_left(p, end);
                if (p == end || *p != constants::char_percent)
                    BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder in the filter string");

                // Skip the closing char_percent
                p = constants::trim_spaces_left(++p, end);

                // If the filter has negation operator, do not expect a relation (i.e. "!%attr% > 1" is not valid because "!%attr%" is interpreted as an attribute presence test)
                if (!negation_present)
                    p = parse_relation(p, end);
                else
                    on_relation_complete();
            }
            else if (c == constants::char_exclamation || scan_keyword(p, end, next, constants::not_keyword()))
            {
                // We found negation operation. Parse the subexpression to be negated.
                negated ^= true;
                negation_present = true;
                p = constants::trim_spaces_left(next, end);
                continue;
            }
            else if (c == constants::char_paren_bracket_left)
            {
                // We found a nested subexpression
                parse(next, end, depth + 1);
                p = next;
            }
            else
            {
                BOOST_LOG_THROW_DESCR(parse_error, "Invalid filter definition: unexpected character");
            }

            if (negated)
                on_negation();

            done = true;

            break;
        }

        if (negation_present && !done)
        {
            // This would happen if a filter consists of a single '!'
            BOOST_LOG_THROW_DESCR(parse_error, "Filter parsing error: negation operator applied to nothingness");
        }

        begin = p;
    }

    //! Parses filtering relation
    iterator_type parse_relation(iterator_type begin, iterator_type end)
    {
        iterator_type p = begin;
        if (p != end)
        {
            iterator_type next = p;
            if (scan_keyword(p, end, next, constants::equal_keyword()))
                m_ComparisonRelation = &filter_factory_type::on_equality_relation;
            else if (scan_keyword(p, end, next, constants::not_equal_keyword()))
                m_ComparisonRelation = &filter_factory_type::on_inequality_relation;
            else if (scan_keyword(p, end, next, constants::greater_keyword()))
                m_ComparisonRelation = &filter_factory_type::on_greater_relation;
            else if (scan_keyword(p, end, next, constants::less_keyword()))
                m_ComparisonRelation = &filter_factory_type::on_less_relation;
            else if (scan_keyword(p, end, next, constants::greater_or_equal_keyword()))
                m_ComparisonRelation = &filter_factory_type::on_greater_or_equal_relation;
            else if (scan_keyword(p, end, next, constants::less_or_equal_keyword()))
                m_ComparisonRelation = &filter_factory_type::on_less_or_equal_relation;
            else
            {
                // Check for custom relation
                while (next != end && (encoding::isalnum(*next) || *next == constants::char_underline))
                    ++next;
                if (p == next)
                    goto DoneL;
                m_CustomRelation.assign(p, next);
            }

            // We have parsed a relation operator, there must be an operand
            next = constants::trim_spaces_left(next, end);
            string_type operand;
            p = constants::parse_operand(next, end, operand);
            if (next == p)
                BOOST_LOG_THROW_DESCR(parse_error, "Missing operand for a relation in the filter string");

            m_Operand = boost::in_place(operand);
        }

    DoneL:
        // The relation can be as simple as a sole attribute placeholder (which means that only attribute presence has to be checked).
        // So regardless how we get here, the relation is parsed completely.
        on_relation_complete();

        return p;
    }

    //! Checks if the string contains a keyword
    static bool scan_keyword(iterator_type begin, iterator_type end, iterator_type& next, iterator_type keyword)
    {
        for (iterator_type p = begin; p != end; ++p, ++keyword)
        {
            char_type c1 = *p, c2 = *keyword;
            if (c2 == 0)
            {
                if (encoding::isspace(c1))
                {
                    next = p;
                    return true;
                }
                break;
            }
            if (c1 != c2)
                break;
        }

        return false;
    }

    //! The attribute name handler
    void on_attribute_name(iterator_type begin, iterator_type end)
    {
        if (begin == end)
            BOOST_LOG_THROW_DESCR(parse_error, "Empty attribute name encountered");
        m_AttributeName = attribute_name(log::aux::to_narrow(string_type(begin, end)));
    }

    //! The comparison relation handler
    void on_relation_complete()
    {
        if (!!m_AttributeName)
        {
            filters_repository< char_type > const& repo = filters_repository< char_type >::get();
            filter_factory_type& factory = repo.get_factory(m_AttributeName);

            if (!!m_Operand)
            {
                if (!!m_ComparisonRelation)
                {
                    m_Subexpressions.push((factory.*m_ComparisonRelation)(m_AttributeName, m_Operand.get()));
                    m_ComparisonRelation = NULL;
                }
                else if (!m_CustomRelation.empty())
                {
                    m_Subexpressions.push(factory.on_custom_relation(m_AttributeName, m_CustomRelation, m_Operand.get()));
                    m_CustomRelation.clear();
                }
                else
                {
                    // This should never happen
                    BOOST_ASSERT_MSG(false, "Filter parser internal error: the attribute name or subexpression operation is not set while trying to construct a relation");
                    BOOST_LOG_THROW_DESCR(parse_error, "Filter parser internal error: the attribute name or subexpression operation is not set while trying to construct a subexpression");
                }

                m_Operand = none;
            }
            else
            {
                // This branch is taken if the relation is a single attribute name, which is recognized as the attribute presence check
                BOOST_ASSERT_MSG(!m_ComparisonRelation && m_CustomRelation.empty(), "Filter parser internal error: the relation operation is set while operand is not");
                m_Subexpressions.push(factory.on_exists_test(m_AttributeName));
            }

            m_AttributeName = attribute_name();
        }
        else
        {
            // This should never happen
            BOOST_ASSERT_MSG(false, "Filter parser internal error: the attribute name is not set while trying to construct a relation");
            BOOST_LOG_THROW_DESCR(parse_error, "Filter parser internal error: the attribute name is not set while trying to construct a relation");
        }
    }

    //! The negation operation handler
    void on_negation()
    {
        if (!m_Subexpressions.empty())
        {
            m_Subexpressions.top() = !phoenix::bind(m_Subexpressions.top(), phoenix::placeholders::_1);
        }
        else
        {
            // This would happen if a filter consists of "!()"
            BOOST_LOG_THROW_DESCR(parse_error, "Filter parsing error: negation operator applied to an empty subexpression");
        }
    }

    //! The logical AND operation handler
    void on_and()
    {
        if (!m_Subexpressions.empty())
        {
            filter right = boost::move(m_Subexpressions.top());
            m_Subexpressions.pop();
            if (!m_Subexpressions.empty())
            {
                filter const& left = m_Subexpressions.top();
                m_Subexpressions.top() = phoenix::bind(left, phoenix::placeholders::_1) && phoenix::bind(right, phoenix::placeholders::_1);
                return;
            }
        }

        // This should never happen
        BOOST_LOG_THROW_DESCR(parse_error, "Filter parser internal error: the subexpression is not set while trying to construct a filter");
    }

    //! The logical OR operation handler
    void on_or()
    {
        if (!m_Subexpressions.empty())
        {
            filter right = boost::move(m_Subexpressions.top());
            m_Subexpressions.pop();
            if (!m_Subexpressions.empty())
            {
                filter const& left = m_Subexpressions.top();
                m_Subexpressions.top() = phoenix::bind(left, phoenix::placeholders::_1) || phoenix::bind(right, phoenix::placeholders::_1);
                return;
            }
        }

        // This should never happen
        BOOST_LOG_THROW_DESCR(parse_error, "Filter parser internal error: the subexpression is not set while trying to construct a filter");
    }

    //  Assignment and copying are prohibited
    BOOST_DELETED_FUNCTION(filter_parser(filter_parser const&))
    BOOST_DELETED_FUNCTION(filter_parser& operator= (filter_parser const&))
};

} // namespace

//! The function registers a filter factory object for the specified attribute name
template< typename CharT >
BOOST_LOG_SETUP_API void register_filter_factory(attribute_name const& name, shared_ptr< filter_factory< CharT > > const& factory)
{
    BOOST_ASSERT(!!name);
    BOOST_ASSERT(!!factory);

    filters_repository< CharT >& repo = filters_repository< CharT >::get();

    BOOST_LOG_EXPR_IF_MT(log::aux::exclusive_lock_guard< log::aux::light_rw_mutex > lock(repo.m_Mutex);)
    repo.m_Map[name] = factory;
}

//! The function parses a filter from the string
template< typename CharT >
BOOST_LOG_SETUP_API filter parse_filter(const CharT* begin, const CharT* end)
{
    typedef CharT char_type;

    filter_parser< char_type > parser;
    const char_type* p = begin;

    BOOST_LOG_EXPR_IF_MT(filters_repository< CharT >& repo = filters_repository< CharT >::get();)
    BOOST_LOG_EXPR_IF_MT(log::aux::shared_lock_guard< log::aux::light_rw_mutex > lock(repo.m_Mutex);)

    parser.parse(p, end);

    return parser.get_filter();
}

#ifdef BOOST_LOG_USE_CHAR

template BOOST_LOG_SETUP_API
void register_filter_factory< char >(attribute_name const& name, shared_ptr< filter_factory< char > > const& factory);
template BOOST_LOG_SETUP_API
filter parse_filter< char >(const char* begin, const char* end);

#endif // BOOST_LOG_USE_CHAR

#ifdef BOOST_LOG_USE_WCHAR_T

template BOOST_LOG_SETUP_API
void register_filter_factory< wchar_t >(attribute_name const& name, shared_ptr< filter_factory< wchar_t > > const& factory);
template BOOST_LOG_SETUP_API
filter parse_filter< wchar_t >(const wchar_t* begin, const wchar_t* end);

#endif // BOOST_LOG_USE_WCHAR_T

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WITHOUT_SETTINGS_PARSERS
