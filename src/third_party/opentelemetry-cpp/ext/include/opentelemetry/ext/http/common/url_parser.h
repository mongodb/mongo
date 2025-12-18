// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace ext
{
namespace http
{
namespace common
{
// http://user:password@host:port/path1/path2?key1=val2&key2=val2
// http://host:port/path1/path2?key1=val1&key2=val2
// host:port/path1
// host:port ( path defaults to "/")
// host:port?

class UrlParser
{
public:
  std::string url_;
  std::string host_;
  std::string scheme_;
  std::string path_;
  uint16_t port_;
  std::string query_;
  bool success_;

  UrlParser(std::string url) : url_(std::move(url)), success_(true)
  {
    if (url_.length() == 0)
    {
      return;
    }
    size_t cpos = 0;
    // scheme
    size_t pos = url_.find("://", cpos);
    if (pos == std::string::npos)
    {
      // scheme missing, use default as http
      scheme_ = "http";
    }
    else
    {
      scheme_ = url_.substr(cpos, pos - cpos);
      cpos    = pos + 3;
    }

    // credentials
    size_t pos1 = url_.find_first_of('@', cpos);
    if (pos1 != std::string::npos)
    {
      size_t pos2 = url_.find_first_of('/', cpos);
      // TODO - handle credentials
      if (pos2 == std::string::npos || pos1 < pos2)
      {
        pos  = pos1;
        cpos = pos1 + 1;
      }
    }
    pos          = FindPortPosition(url_, cpos);
    bool is_port = false;
    if (pos == std::string::npos)
    {
      // port missing. Used default 80 / 443
      if (scheme_ == "http")
      {
        port_ = 80;
      }
      else if (scheme_ == "https")
      {
        port_ = 443;
      }
    }
    else
    {
      // port present
      is_port = true;
      host_   = url_.substr(cpos, pos - cpos);
      cpos    = pos + 1;
    }
    pos = url_.find_first_of("/?", cpos);
    if (pos == std::string::npos)
    {
      path_ = std::string("/");  // use default path
      if (is_port)
      {
        auto port_str = url_.substr(cpos);
        port_         = GetPort(port_str);
      }
      else
      {
        host_ = url_.substr(cpos);
      }
      return;
    }
    if (is_port)
    {
      auto port_str = url_.substr(cpos, pos - cpos);
      port_         = GetPort(port_str);
    }
    else
    {
      host_ = url_.substr(cpos, pos - cpos);
    }
    cpos = pos;

    if (url_[cpos] == '/')
    {
      pos = url_.find('?', cpos);
      if (pos == std::string::npos)
      {
        path_ = url_.substr(cpos);
      }
      else
      {
        path_  = url_.substr(cpos, pos - cpos);
        cpos   = pos + 1;
        query_ = url_.substr(cpos);
      }
      return;
    }
    path_ = std::string("/");
    if (url_[cpos] == '?')
    {
      query_ = url_.substr(cpos);
    }
  }

private:
  static std::string::size_type FindPortPosition(const std::string &url,
                                                 std::string::size_type offset)
  {
    // @see https://www.rfc-editor.org/rfc/rfc3986#page-18
    size_t sub_expression_counter = 0;
    for (std::string::size_type i = offset; i < url.size(); ++i)
    {
      char c = url[i];
      if (0 == sub_expression_counter && c == ':')
      {
        return i;
      }

      if (c == '[')
      {
        ++sub_expression_counter;
      }
      else if (c == ']')
      {
        if (sub_expression_counter > 0)
        {
          --sub_expression_counter;
        }
      }
      else if (0 == sub_expression_counter && c == '/')
      {
        break;
      }
    }

    return std::string::npos;
  }

  std::uint16_t GetPort(const std::string &s)
  {
    char *e   = nullptr;
    errno     = 0;
    auto port = std::strtol(s.c_str(), &e, 10);
    if (e == s.c_str() || e != s.c_str() + s.size() || errno == ERANGE || port < 0 || port > 65535)
    {
      success_ = false;
      return 0;
    }

    return static_cast<uint16_t>(port);
  }
};

class UrlDecoder
{
public:
  static std::string Decode(const std::string &encoded)
  {
    std::string result;
    result.reserve(encoded.size());

    auto hex_to_int = [](int ch) -> int {
      if (ch >= '0' && ch <= '9')
        return ch - '0';
      if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
      if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
      return -1;
    };

    for (size_t pos = 0; pos < encoded.size(); pos++)
    {
      auto c = encoded[pos];
      if (c == '%')
      {
        if (pos + 2 >= encoded.size())
        {
          return encoded;
        }

        int hi = hex_to_int(encoded[pos + 1]);
        int lo = hex_to_int(encoded[pos + 2]);

        if (hi == -1 || lo == -1)
        {
          return encoded;
        }

        c = static_cast<char>((hi << 4) | lo);
        pos += 2;
      }

      result.push_back(c);
    }

    return result;
  }
};

}  // namespace common

}  // namespace http
}  // namespace ext
OPENTELEMETRY_END_NAMESPACE
