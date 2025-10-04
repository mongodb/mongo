//
// local/detail/impl/endpoint.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
// Derived from a public domain implementation written by Daniel Casimiro.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_LOCAL_DETAIL_IMPL_ENDPOINT_IPP
#define ASIO_LOCAL_DETAIL_IMPL_ENDPOINT_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#if defined(ASIO_HAS_LOCAL_SOCKETS)

#include <cstring>
#include "asio/detail/socket_ops.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"
#include "asio/local/detail/endpoint.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace local {
namespace detail {

endpoint::endpoint() noexcept
{
  init("", 0);
}

endpoint::endpoint(const char* path_name)
{
  using namespace std; // For strlen.
  init(path_name, strlen(path_name));
}

endpoint::endpoint(const std::string& path_name)
{
  init(path_name.data(), path_name.length());
}

#if defined(ASIO_HAS_STRING_VIEW)
endpoint::endpoint(string_view path_name)
{
  init(path_name.data(), path_name.length());
}
#endif // defined(ASIO_HAS_STRING_VIEW)

void endpoint::resize(std::size_t new_size)
{
  if (new_size > sizeof(asio::detail::sockaddr_un_type))
  {
    asio::error_code ec(asio::error::invalid_argument);
    asio::detail::throw_error(ec);
  }
  else if (new_size == 0)
  {
    size_ = offsetof(asio::detail::sockaddr_un_type, sun_path);
  }
  else
  {
    size_ = new_size;
  }
}

std::string endpoint::path() const
{
  // There are three cases:
  // 1. The path is empty.
  // 2. The path is a non-empty null-terminated string.
  // 3. The path begins with a null character (Linux's "abstract" addresses).
  const auto offset = offsetof(asio::detail::sockaddr_un_type, sun_path);
  std::size_t path_length;
  if (size_ <= offset)
    path_length = 0;
  else if (size_ > offset && data_.local.sun_path[0] != '\0')
    path_length = size_ - offset - 1; // exclude null terminator
  else path_length = size_ - offset;
  return std::string(data_.local.sun_path, path_length);
}

void endpoint::path(const char* p)
{
  using namespace std; // For strlen.
  init(p, strlen(p));
}

void endpoint::path(const std::string& p)
{
  init(p.data(), p.length());
}

bool operator==(const endpoint& e1, const endpoint& e2) noexcept
{
  return e1.path() == e2.path();
}

bool operator<(const endpoint& e1, const endpoint& e2) noexcept
{
  return e1.path() < e2.path();
}

void endpoint::init(const char* path_name, std::size_t path_length)
{
  if (path_length > sizeof(data_.local.sun_path) - 1)
  {
    // The buffer is not large enough to store this address.
    asio::error_code ec(asio::error::name_too_long);
    asio::detail::throw_error(ec);
  }

  using namespace std; // For memset and memcpy.
  memset(&data_.local, 0, sizeof(asio::detail::sockaddr_un_type));
  data_.local.sun_family = AF_UNIX;
  const auto offset = offsetof(asio::detail::sockaddr_un_type, sun_path);
  if (path_length > 0)
  {
    memcpy(data_.local.sun_path, path_name, path_length);
    // Linux "abstract" addresses begin with a null byte and do not have a null
    // terminator. File system path addresses include a null terminator.
    if (data_.local.sun_path[0] == '\0')
      size_ = offset + path_length;
    else
      size_ = offset + path_length + 1;
  }
  else
    size_ = offset;

}

} // namespace detail
} // namespace local
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // defined(ASIO_HAS_LOCAL_SOCKETS)

#endif // ASIO_LOCAL_DETAIL_IMPL_ENDPOINT_IPP
