/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   default_filter_factory.hpp
 * \author Andrey Semashev
 * \date   29.05.2010
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_DEFAULT_FILTER_FACTORY_HPP_INCLUDED_
#define BOOST_DEFAULT_FILTER_FACTORY_HPP_INCLUDED_

#include <boost/log/detail/setup_config.hpp>
#include <cstddef>
#include <boost/log/attributes/attribute_name.hpp>
#include <boost/log/attributes/value_visitation.hpp>
#include <boost/log/utility/setup/filter_parser.hpp>
#include <boost/log/utility/functional/save_result.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! Relation predicate wrapper
template< typename ValueT, typename PredicateT >
struct predicate_wrapper
{
    typedef typename PredicateT::result_type result_type;

    explicit predicate_wrapper(attribute_name const& name, PredicateT const& pred) : m_name(name), m_visitor(pred)
    {
    }

    template< typename T >
    result_type operator() (T const& arg) const
    {
        bool res = false;
        boost::log::visit< ValueT >(m_name, arg, save_result_wrapper< PredicateT const&, bool >(m_visitor, res));
        return res;
    }

private:
    attribute_name m_name;
    const PredicateT m_visitor;
};

//! The default filter factory that supports creating filters for the standard types (see utility/type_dispatch/standard_types.hpp)
template< typename CharT >
class default_filter_factory :
    public filter_factory< CharT >
{
private:
    //! Base type
    typedef filter_factory< CharT > base_type;
    //! Self type
    typedef default_filter_factory< CharT > this_type;

public:
    //  Type imports
    typedef typename base_type::char_type char_type;
    typedef typename base_type::string_type string_type;

    //! The callback for equality relation filter
    filter on_equality_relation(attribute_name const& name, string_type const& arg) BOOST_OVERRIDE;
    //! The callback for inequality relation filter
    filter on_inequality_relation(attribute_name const& name, string_type const& arg) BOOST_OVERRIDE;
    //! The callback for less relation filter
    filter on_less_relation(attribute_name const& name, string_type const& arg) BOOST_OVERRIDE;
    //! The callback for greater relation filter
    filter on_greater_relation(attribute_name const& name, string_type const& arg) BOOST_OVERRIDE;
    //! The callback for less or equal relation filter
    filter on_less_or_equal_relation(attribute_name const& name, string_type const& arg) BOOST_OVERRIDE;
    //! The callback for greater or equal relation filter
    filter on_greater_or_equal_relation(attribute_name const& name, string_type const& arg) BOOST_OVERRIDE;

    //! The callback for custom relation filter
    filter on_custom_relation(attribute_name const& name, string_type const& rel, string_type const& arg) BOOST_OVERRIDE;

private:
    //! The function parses the argument value for a binary relation and constructs the corresponding filter
    template< typename RelationT >
    static filter parse_argument(attribute_name const& name, string_type const& arg);
};

//! The function parses the "matches" relation
template< typename CharT >
filter parse_matches_relation(attribute_name const& name, std::basic_string< CharT > const& operand);

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_DEFAULT_FILTER_FACTORY_HPP_INCLUDED_
