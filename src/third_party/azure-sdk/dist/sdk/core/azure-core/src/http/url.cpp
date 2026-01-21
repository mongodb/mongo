// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/url.hpp"

#include "azure/core/internal/strings.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

using namespace Azure::Core;

Url::Url(std::string const& url)
{
  auto urlIter = url.cbegin();

  {
    std::string const SchemeEnd = "://";
    auto const schemePos = url.find(SchemeEnd);

    if (schemePos != std::string::npos)
    {
      m_scheme = _internal::StringExtensions::ToLower(url.substr(0, schemePos));
      urlIter += schemePos + SchemeEnd.length();
    }
  }

  {
    auto const hostIter
        = std::find_if(urlIter, url.end(), [](auto c) { return c == '/' || c == '?' || c == ':'; });

    m_host = std::string(urlIter, hostIter);
    urlIter = hostIter;
  }

  if (urlIter == url.end())
  {
    return;
  }

  if (*urlIter == ':')
  {
    ++urlIter;
    auto const portIter
        = std::find_if_not(urlIter, url.end(), _internal::StringExtensions::IsDigit);

    auto const portNumber = std::stoi(std::string(urlIter, portIter));

    // stoi will throw out_of_range when `int` is overflow, but we need to throw if uint16 is
    // overflow
    {
      constexpr auto const MaxPortNumberSupported = (std::numeric_limits<uint16_t>::max)();
      if (portNumber > MaxPortNumberSupported)
      {
        throw std::out_of_range(
            "The port number is out of range. The max supported number is "
            + std::to_string(MaxPortNumberSupported) + ".");
      }
    }

    // cast is safe because the overflow was detected before
    m_port = static_cast<uint16_t>(portNumber);
    urlIter = portIter;
  }

  if (urlIter == url.end())
  {
    return;
  }

  if (*urlIter != '/' && *urlIter != '?')
  {
    // only char '/' or '?' is valid after the port (or the end of the URL). Any other char is an
    // invalid input
    throw std::invalid_argument("The port number contains invalid characters.");
  }

  if (*urlIter == '/')
  {
    ++urlIter;

    auto const pathIter = std::find(urlIter, url.end(), '?');
    m_encodedPath = std::string(urlIter, pathIter);

    urlIter = pathIter;
  }

  if (urlIter != url.end() && *urlIter == '?')
  {
    ++urlIter;
    AppendQueryParameters(std::string(urlIter, std::find(urlIter, url.end(), '#')));
  }
}

std::string Url::Decode(std::string const& value)
{
  std::string decodedValue;
  auto const valueSize = value.size();
  for (size_t i = 0; i < valueSize; ++i)
  {
    auto const c = value[i];
    switch (c)
    {
      case '%':
        if ((valueSize - i) < 3 // need at least 3 characters: "%XY"
            || !_internal::StringExtensions::IsHexDigit(value[i + 1])
            || !_internal::StringExtensions::IsHexDigit(value[i + 2]))
        {
          throw std::runtime_error("failed when decoding URL component");
        }

        decodedValue += static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
        i += 2;
        break;

      case '+':
        decodedValue += ' ';
        break;

      default:
        decodedValue += c;
        break;
    }
  }

  return decodedValue;
}

namespace {
bool ShouldEncode(char c)
{
  static std::unordered_set<char> const ExtraNonEncodableChars = {'-', '.', '_', '~'};

  return !_internal::StringExtensions::IsAlphaNumeric(c)
      && ExtraNonEncodableChars.find(c) == ExtraNonEncodableChars.end();
}
} // namespace

std::string Url::Encode(const std::string& value, const std::string& doNotEncodeSymbols)
{
  auto const Hex = "0123456789ABCDEF";

  std::unordered_set<char> const doNotEncodeSymbolsSet(
      doNotEncodeSymbols.begin(), doNotEncodeSymbols.end());

  std::string encoded;
  for (auto const c : value)
  {
    // encode if char is not in the default non-encoding set AND if it is NOT in chars to ignore
    // from user input
    if (ShouldEncode(c) && doNotEncodeSymbolsSet.find(c) == doNotEncodeSymbolsSet.end())
    {
      auto const u8 = static_cast<uint8_t>(c);

      encoded += '%';
      encoded += Hex[(u8 >> 4) & 0x0f];
      encoded += Hex[u8 & 0x0f];
    }
    else
    {
      encoded += c;
    }
  }

  return encoded;
}

void Url::AppendQueryParameters(const std::string& query)
{
  std::string::const_iterator cur = query.begin();
  if (cur != query.end() && *cur == '?')
  {
    ++cur;
  }

  while (cur != query.end())
  {
    auto key_end = std::find(cur, query.end(), '=');
    std::string query_key = std::string(cur, key_end);

    cur = key_end;
    if (cur != query.end())
    {
      ++cur;
    }

    auto value_end = std::find(cur, query.end(), '&');
    std::string query_value = std::string(cur, value_end);

    cur = value_end;
    if (cur != query.end())
    {
      ++cur;
    }
    m_encodedQueryParameters[std::move(query_key)] = std::move(query_value);
  }
}

std::string Url::GetUrlWithoutQuery(bool relative) const
{
  std::string url;

  if (!relative)
  {
    if (!m_scheme.empty())
    {
      url += m_scheme + "://";
    }
    url += m_host;
    if (m_port != 0)
    {
      url += ":" + std::to_string(m_port);
    }
  }

  if (!m_encodedPath.empty())
  {
    if (!relative)
    {
      if (m_encodedPath[0] != '/')
      {
        url += "/";
      }
    }

    url += m_encodedPath;
  }

  return url;
}

std::string Url::GetRelativeUrl() const
{
  return GetUrlWithoutQuery(true)
      + _detail::FormatEncodedUrlQueryParameters(m_encodedQueryParameters);
}

std::string Url::GetAbsoluteUrl() const
{
  return GetUrlWithoutQuery(false)
      + _detail::FormatEncodedUrlQueryParameters(m_encodedQueryParameters);
}
