// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/http.hpp"

#include "azure/core/http/policies/policy.hpp"
#include "azure/core/internal/strings.hpp"
#include "azure/core/url.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

using namespace Azure::Core;
using namespace Azure::Core::Http;

char const Azure::Core::Http::_internal::HttpShared::ContentType[] = "content-type";
char const Azure::Core::Http::_internal::HttpShared::ApplicationJson[] = "application/json";
char const Azure::Core::Http::_internal::HttpShared::Accept[] = "accept";
char const Azure::Core::Http::_internal::HttpShared::MsRequestId[] = "x-ms-request-id";
char const Azure::Core::Http::_internal::HttpShared::MsClientRequestId[] = "x-ms-client-request-id";

const HttpMethod HttpMethod::Get("GET");
const HttpMethod HttpMethod::Head("HEAD");
const HttpMethod HttpMethod::Post("POST");
const HttpMethod HttpMethod::Put("PUT");
const HttpMethod HttpMethod::Delete("DELETE");
const HttpMethod HttpMethod::Patch("PATCH");
const HttpMethod HttpMethod::Options("OPTIONS");

namespace {
bool IsInvalidHeaderNameChar(char c)
{
  static std::unordered_set<char> const HeaderNameExtraValidChars
      = {' ', '!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'};

  return !Azure::Core::_internal::StringExtensions::IsAlphaNumeric(c)
      && HeaderNameExtraValidChars.find(c) == HeaderNameExtraValidChars.end();
}
} // namespace

void Azure::Core::Http::_detail::RawResponseHelpers::InsertHeaderWithValidation(
    Azure::Core::CaseInsensitiveMap& headers,
    std::string const& headerName,
    std::string const& headerValue)
{

  // Check all chars in name are valid
  if (std::find_if(headerName.begin(), headerName.end(), IsInvalidHeaderNameChar)
      != headerName.end())
  {
    throw std::invalid_argument("Invalid header name: " + headerName);
  }

  // insert (override if duplicated)
  headers[headerName] = headerValue;
}
