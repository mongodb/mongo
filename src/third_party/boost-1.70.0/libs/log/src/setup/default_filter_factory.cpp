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

#if !defined(BOOST_LOG_WITHOUT_SETTINGS_PARSERS) && !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)

#include <boost/log/detail/setup_config.hpp>
#include <string>
#include <boost/mpl/vector.hpp>
#include <boost/preprocessor/seq/enum.hpp>
#include <boost/spirit/include/qi_core.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/expressions/predicates/has_attr.hpp>
#include <boost/log/utility/type_dispatch/standard_types.hpp>
#include <boost/log/utility/string_literal.hpp>
#include <boost/log/utility/functional/logical.hpp>
#include <boost/log/utility/functional/begins_with.hpp>
#include <boost/log/utility/functional/ends_with.hpp>
#include <boost/log/utility/functional/contains.hpp>
#include <boost/log/detail/code_conversion.hpp>
#if defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)
#include <boost/fusion/container/set.hpp>
#include <boost/fusion/sequence/intrinsic/at_key.hpp>
#include <boost/fusion/algorithm/iteration/for_each.hpp>
#endif
#include "default_filter_factory.hpp"
#include "parser_utils.hpp"
#include <boost/log/detail/header.hpp>

namespace qi = boost::spirit::qi;

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

#if defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)

//! A special filtering predicate that adopts the string operand to the attribute value character type
template< typename RelationT >
class string_predicate :
    public RelationT
{
private:
    template< typename StringT >
    struct initializer
    {
        typedef void result_type;

        explicit initializer(StringT const& val) : m_initializer(val)
        {
        }

        template< typename T >
        result_type operator() (T& val) const
        {
            try
            {
                log::aux::code_convert(m_initializer, val);
            }
            catch (...)
            {
                val.clear();
            }
        }

    private:
        StringT const& m_initializer;
    };

public:
    typedef RelationT relation_type;
    typedef typename relation_type::result_type result_type;

    template< typename StringT >
    string_predicate(relation_type const& rel, StringT const& operand) : relation_type(rel)
    {
        fusion::for_each(m_operands, initializer< StringT >(operand));
    }

    template< typename T >
    result_type operator() (T const& val) const
    {
        typedef std::basic_string< typename T::value_type > operand_type;
        return relation_type::operator() (val, fusion::at_key< operand_type >(m_operands));
    }

private:
    fusion::set< std::string, std::wstring > m_operands;
};

#else // defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)

//! A special filtering predicate that adopts the string operand to the attribute value character type
template< typename RelationT >
class string_predicate :
    public RelationT
{
private:
#if defined(BOOST_LOG_USE_CHAR)
    typedef std::basic_string< char > string_type;
#elif defined(BOOST_LOG_USE_WCHAR_T)
    typedef std::basic_string< wchar_t > string_type;
#else
#error Boost.Log: Inconsistent character configuration
#endif

public:
    typedef RelationT relation_type;
    typedef typename relation_type::result_type result_type;

public:
    string_predicate(relation_type const& rel, string_type const& operand) : relation_type(rel), m_operand(operand)
    {
    }

    template< typename T >
    result_type operator() (T const& val) const
    {
        return relation_type::operator() (val, m_operand);
    }

private:
    const string_type m_operand;
};

#endif // defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)

//! A filtering predicate for numeric relations
template< typename NumericT, typename RelationT >
class numeric_predicate :
    public string_predicate< RelationT >
{
    typedef string_predicate< RelationT > base_type;

public:
    typedef NumericT numeric_type;
    typedef typename base_type::relation_type relation_type;
    typedef typename base_type::result_type result_type;

public:
    template< typename StringT >
    numeric_predicate(relation_type const& rel, StringT const& string_operand, numeric_type numeric_operand) :
        base_type(rel, string_operand),
        m_numeric_operand(numeric_operand)
    {
    }

    template< typename CharT, typename TraitsT, typename AllocatorT >
    result_type operator() (std::basic_string< CharT, TraitsT, AllocatorT > const& val) const
    {
        return base_type::operator() (val);
    }

    template< typename CharT, typename TraitsT >
    result_type operator() (basic_string_literal< CharT, TraitsT > const& val) const
    {
        return base_type::operator() (val);
    }

    template< typename T >
    result_type operator() (T const& val) const
    {
        return relation_type::operator() (val, m_numeric_operand);
    }

private:
    const numeric_type m_numeric_operand;
};

typedef mpl::vector<
    BOOST_PP_SEQ_ENUM(BOOST_LOG_STANDARD_FLOATING_POINT_TYPES()BOOST_LOG_STANDARD_STRING_TYPES())
> floating_point_and_string_types;

} // namespace

//! The callback for equality relation filter
template< typename CharT >
filter default_filter_factory< CharT >::on_equality_relation(attribute_name const& name, string_type const& arg)
{
    return parse_argument< equal_to >(name, arg);
}

//! The callback for inequality relation filter
template< typename CharT >
filter default_filter_factory< CharT >::on_inequality_relation(attribute_name const& name, string_type const& arg)
{
    return parse_argument< not_equal_to >(name, arg);
}

//! The callback for less relation filter
template< typename CharT >
filter default_filter_factory< CharT >::on_less_relation(attribute_name const& name, string_type const& arg)
{
    return parse_argument< less >(name, arg);
}

//! The callback for greater relation filter
template< typename CharT >
filter default_filter_factory< CharT >::on_greater_relation(attribute_name const& name, string_type const& arg)
{
    return parse_argument< greater >(name, arg);
}

//! The callback for less or equal relation filter
template< typename CharT >
filter default_filter_factory< CharT >::on_less_or_equal_relation(attribute_name const& name, string_type const& arg)
{
    return parse_argument< less_equal >(name, arg);
}

//! The callback for greater or equal relation filter
template< typename CharT >
filter default_filter_factory< CharT >::on_greater_or_equal_relation(attribute_name const& name, string_type const& arg)
{
    return parse_argument< greater_equal >(name, arg);
}

//! The callback for custom relation filter
template< typename CharT >
filter default_filter_factory< CharT >::on_custom_relation(attribute_name const& name, string_type const& rel, string_type const& arg)
{
    typedef log::aux::char_constants< char_type > constants;

    if (rel == constants::begins_with_keyword())
    {
        typedef string_predicate< begins_with_fun > predicate;
        return predicate_wrapper< log::string_types, predicate >(name, predicate(begins_with_fun(), arg));
    }
    else if (rel == constants::ends_with_keyword())
    {
        typedef string_predicate< ends_with_fun > predicate;
        return predicate_wrapper< log::string_types, predicate >(name, predicate(ends_with_fun(), arg));
    }
    else if (rel == constants::contains_keyword())
    {
        typedef string_predicate< contains_fun > predicate;
        return predicate_wrapper< log::string_types, predicate >(name, predicate(contains_fun(), arg));
    }
    else if (rel != constants::matches_keyword())
    {
        BOOST_LOG_THROW_DESCR(parse_error, "The custom attribute relation \"" + log::aux::to_narrow(rel) + "\" is not supported");
    }

    return parse_matches_relation(name, arg);
}


//! The function parses the argument value for a binary relation and constructs the corresponding filter
template< typename CharT >
template< typename RelationT >
filter default_filter_factory< CharT >::parse_argument(attribute_name const& name, string_type const& arg)
{
    const char_type* begin = arg.c_str();
    const char_type* const end = begin + arg.size();

    double real_val = 0.0;
    const qi::real_parser< double, qi::strict_real_policies< double > > real_;
    if (qi::parse(begin, end, real_, real_val) && begin == end)
    {
        typedef numeric_predicate< double, RelationT > predicate;
        typedef floating_point_and_string_types value_types;
        return predicate_wrapper< value_types, predicate >(name, predicate(RelationT(), arg, real_val));
    }
    else
    {
        begin = arg.c_str();
        long int_val = 0;
        if (qi::parse(begin, end, qi::long_, int_val) && begin == end)
        {
            typedef numeric_predicate< long, RelationT > predicate;
            typedef default_attribute_types value_types;
            return predicate_wrapper< value_types, predicate >(name, predicate(RelationT(), arg, int_val));
        }
        else
        {
            typedef string_predicate< RelationT > predicate;
            return predicate_wrapper< log::string_types, predicate >(name, predicate(RelationT(), arg));
        }
    }
}

//  Explicitly instantiate factory implementation
#ifdef BOOST_LOG_USE_CHAR
template class default_filter_factory< char >;
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
template class default_filter_factory< wchar_t >;
#endif

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // !defined(BOOST_LOG_WITHOUT_SETTINGS_PARSERS) && !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)
