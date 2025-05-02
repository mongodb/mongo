// Copyright (c) 2021 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_POSIX)

#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/cstring_ref.hpp>

#include <unistd.h>
#include <cstring>


BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace environment
{
namespace detail
{

basic_cstring_ref<char_type, value_char_traits<char>> get(
        basic_cstring_ref<char_type, key_char_traits<char_type>> key,
        error_code & ec)
{
    auto res = ::getenv(key.c_str());
    if (res == nullptr)
    {
        BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOENT, system_category());
        return {};
    }
    return res;
}

void set(basic_cstring_ref<char_type,   key_char_traits<char_type>>   key,
         basic_cstring_ref<char_type, value_char_traits<char_type>> value,
                error_code & ec)
{
    if (::setenv(key.c_str(), value.c_str(), true))
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}

void unset(basic_cstring_ref<char_type, key_char_traits<char_type>> key, error_code & ec)
{
    if (::unsetenv(key.c_str()))
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}


native_handle_type load_native_handle() { return environ; }


native_iterator next(native_iterator nh)
{
    return nh + 1;
}

native_iterator find_end(native_handle_type nh)
{
    while (*nh != nullptr)
        nh++;
    return nh;
}

bool has_x_access(const char * pth)
{
  return (::access(pth, X_OK) == 0);
}

}
}
BOOST_PROCESS_V2_END_NAMESPACE

#endif
