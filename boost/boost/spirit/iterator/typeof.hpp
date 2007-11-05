/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_ITERATOR_TYPEOF_HPP)
#define BOOST_SPIRIT_ITERATOR_TYPEOF_HPP

#include <boost/typeof/typeof.hpp>

#include <boost/spirit/iterator/multi_pass_fwd.hpp>
#include <boost/spirit/iterator/file_iterator_fwd.hpp>
#include <boost/spirit/iterator/position_iterator_fwd.hpp>

#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()

namespace boost { namespace spirit {

    // external (from core)
    struct nil_t;

    // fixed_size_queue.hpp
    template<typename T, std::size_t N> class fixed_size_queue;
    template<typename QueueT, typename T, typename PointerT>
    class fsq_iterator;

}} // namespace boost::spirit



#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()


#if !defined(BOOST_SPIRIT_NIL_T_TYPEOF_REGISTERED)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::nil_t)
#   define BOOST_SPIRIT_NIL_T_TYPEOF_REGISTERED
#endif


// multi_pass.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::multi_pass,5)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::multi_pass_policies::ref_counted)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::multi_pass_policies::first_owner)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::multi_pass_policies::buf_id_check)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::multi_pass_policies::no_check)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::multi_pass_policies::std_deque)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::multi_pass_policies::fixed_size_queue,(BOOST_TYPEOF_INTEGRAL(std::size_t)))
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::multi_pass_policies::input_iterator)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::multi_pass_policies::lex_input)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::multi_pass_policies::functor_input)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::multi_pass,3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::multi_pass,1)


// file_iterator.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::file_iterator,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::fileiter_impl::std_file_iterator,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::fileiter_impl::mmap_file_iterator,1)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::fileiter_impl::std_file_iterator<char>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::fileiter_impl::std_file_iterator<wchar_t>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::fileiter_impl::mmap_file_iterator<char>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::fileiter_impl::mmap_file_iterator<wchar_t>)


// fixed_size_queue.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::fixed_size_queue,(typename)(BOOST_TYPEOF_INTEGRAL(std::size_t)))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::fsq_iterator,3)


// position_iterator.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::position_iterator,3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::position_iterator2,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::position_policy,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::file_position_base,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::file_position_without_column_base,1)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::file_position)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::file_position_base<std::basic_string<wchar_t> >)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::file_position_without_column)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::file_position_without_column_base<std::basic_string<wchar_t> >)

#endif

