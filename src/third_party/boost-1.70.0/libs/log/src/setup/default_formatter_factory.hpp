/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   default_formatter_factory.hpp
 * \author Andrey Semashev
 * \date   14.07.2013
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_DEFAULT_FORMATTER_FACTORY_HPP_INCLUDED_
#define BOOST_DEFAULT_FORMATTER_FACTORY_HPP_INCLUDED_

#include <boost/log/detail/setup_config.hpp>
#include <boost/log/attributes/attribute_name.hpp>
#include <boost/log/utility/setup/formatter_parser.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! The default filter factory that supports creating filters for the standard types (see utility/type_dispatch/standard_types.hpp)
template< typename CharT >
class default_formatter_factory :
    public formatter_factory< CharT >
{
    //! Base type
    typedef formatter_factory< CharT > base_type;
    //! Self type
    typedef default_formatter_factory< CharT > this_type;

public:
    //  Type imports
    typedef typename base_type::char_type char_type;
    typedef typename base_type::string_type string_type;
    typedef typename base_type::formatter_type formatter_type;
    typedef typename base_type::args_map args_map;

    //! The function creates a formatter for the specified attribute.
    virtual formatter_type create_formatter(attribute_name const& name, args_map const& args);
};

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_DEFAULT_FORMATTER_FACTORY_HPP_INCLUDED_
