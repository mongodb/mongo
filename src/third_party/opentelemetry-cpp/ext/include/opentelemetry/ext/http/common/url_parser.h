// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdlib.h>
#include <cstdint>
#include <string>

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

  UrlParser(std::string url) : url_(url), success_(true)
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
      scheme_ = std::string(url_.begin() + cpos, url_.begin() + pos);
      cpos    = pos + 3;
    }

    // credentials
    size_t pos1 = url_.find_first_of("@", cpos);
    size_t pos2 = url_.find_first_of("/", cpos);
    if (pos1 != std::string::npos)
    {
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
        port_ = 80;
      if (scheme_ == "https")
        port_ = 443;
    }
    else
    {
      // port present
      is_port = true;
      host_   = std::string(url_.begin() + cpos, url_.begin() + pos);
      cpos    = pos + 1;
    }
    pos = url_.find_first_of("/?", cpos);
    if (pos == std::string::npos)
    {
      path_ = std::string("/");  // use default path
      if (is_port)
      {
        port_ = static_cast<uint16_t>(
            std::stoi(std::string(url_.begin() + cpos, url_.begin() + url_.length())));
      }
      else
      {
        host_ = std::string(url_.begin() + cpos, url_.begin() + url_.length());
      }
      return;
    }
    if (is_port)
    {
      port_ =
          static_cast<uint16_t>(std::stoi(std::string(url_.begin() + cpos, url_.begin() + pos)));
    }
    else
    {
      host_ = std::string(url_.begin() + cpos, url_.begin() + pos);
    }
    cpos = pos;

    if (url_[cpos] == '/')
    {
      pos = url_.find('?', cpos);
      if (pos == std::string::npos)
      {
        path_  = std::string(url_.begin() + cpos, url_.begin() + url_.length());
        query_ = "";
      }
      else
      {
        path_  = std::string(url_.begin() + cpos, url_.begin() + pos);
        cpos   = pos + 1;
        query_ = std::string(url_.begin() + cpos, url_.begin() + url_.length());
      }
      return;
    }
    path_ = std::string("/");
    if (url_[cpos] == '?')
    {
      query_ = std::string(url_.begin() + cpos, url_.begin() + url_.length());
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
};

class UrlDecoder
{
public:
  static std::string Decode(const std::string &encoded)
  {
    std::string result;
    result.reserve(encoded.size());

    for (size_t pos = 0; pos < encoded.size(); pos++)
    {
      if (encoded[pos] == '%')
      {

        // Invalid input: less than two characters left after '%'
        if (encoded.size() < pos + 3)
        {
          return encoded;
        }

        char hex[3] = {0};
        hex[0]      = encoded[++pos];
        hex[1]      = encoded[++pos];

        char *endptr;
        long value = strtol(hex, &endptr, 16);

        // Invalid input: no valid hex characters after '%'
        if (endptr != &hex[2])
        {
          return encoded;
        }

        result.push_back(static_cast<char>(value));
      }
      else
      {
        result.push_back(encoded[pos]);
      }
    }

    return result;
  }
};

}  // namespace common

}  // namespace http
}  // namespace ext
OPENTELEMETRY_END_NAMESPACE
