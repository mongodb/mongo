// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_DETAIL_UTF8_HPP
#define BOOST_PROCESS_V2_DETAIL_UTF8_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/throw_error.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail
{

BOOST_PROCESS_V2_DECL std::size_t size_as_utf8(const wchar_t * in, std::size_t size, error_code & ec);
BOOST_PROCESS_V2_DECL std::size_t size_as_wide(const  char   * in, std::size_t size, error_code & ec);

BOOST_PROCESS_V2_DECL std::size_t convert_to_utf8(const wchar_t * in, std::size_t size, 
                                                   char   * out, std::size_t max_size, error_code & ec);
BOOST_PROCESS_V2_DECL std::size_t convert_to_wide(const  char   * in, std::size_t size,  
                                                  wchar_t * out, std::size_t max_size, error_code & ec);

template<typename CharOut, typename Traits = std::char_traits<CharOut>, 
         typename Allocator = std::allocator<CharOut>, typename CharIn,
         typename = typename std::enable_if<std::is_same<CharOut, CharIn>::value>::type> 
std::basic_string<CharOut, Traits, Allocator> conv_string(
    const CharIn * data, std::size_t size, 
    const Allocator allocator = Allocator{})
{
    return std::basic_string<CharOut, Traits, Allocator>(data, size, allocator);
}


template<typename CharOut, typename Traits = std::char_traits<CharOut>, 
         typename Allocator = std::allocator<CharOut>,
         typename = typename std::enable_if<std::is_same<CharOut, char>::value>::type> 
std::basic_string<CharOut, Traits, Allocator> conv_string(
    const wchar_t * data, std::size_t size, 
    const Allocator allocator = Allocator{})
{
    error_code ec;
    const auto req_size = size_as_utf8(data, size, ec);
    if (ec)
        detail::throw_error(ec, "size_as_utf8");

    std::basic_string<CharOut, Traits, Allocator> res(allocator);
    res.resize(req_size);

    auto res_size = convert_to_utf8(data, size, &res.front(), req_size,  ec);
    if (ec)
        detail::throw_error(ec, "convert_to_utf8");

    res.resize(res_size);
    return res;
}

template<typename CharOut, typename Traits = std::char_traits<CharOut>, 
         typename Allocator = std::allocator<CharOut>,
         typename = typename std::enable_if<std::is_same<CharOut, wchar_t>::value>::type> 
std::basic_string<CharOut, Traits, Allocator> conv_string(
    const char * data, std::size_t size, 
    const Allocator allocator = Allocator{})
{
    error_code ec;
    const auto req_size = size_as_wide(data, size, ec);
    if (ec)
        detail::throw_error(ec, "size_as_wide");

    std::basic_string<CharOut, Traits, Allocator> res(allocator);
    res.resize(req_size);

    auto res_size = convert_to_wide(data, size, &res.front(), req_size,  ec);
    if (ec)
        detail::throw_error(ec, "convert_to_wide");

    res.resize(res_size);
    return res;
}

}

BOOST_PROCESS_V2_END_NAMESPACE


#endif //BOOST_PROCESS_V2_DETAIL_UTF8_HPP
