// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/exception.hpp"

#include "azure/core/http/http.hpp"
#include "azure/core/http/policies/policy.hpp"
#include "azure/core/internal/json/json.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace Azure::Core::Http::_internal;

namespace Azure { namespace Core {

  RequestFailedException::RequestFailedException(std::string const& what) : std::runtime_error(what)
  {
  }

  RequestFailedException::RequestFailedException(
      std::unique_ptr<Azure::Core::Http::RawResponse>& rawResponse)
      : std::runtime_error(GetRawResponseErrorMessage(rawResponse)),
        RawResponse(std::move(rawResponse)),
        // These are guaranteed to always be present in the rawResponse.
        StatusCode(RawResponse->GetStatusCode()), ReasonPhrase(RawResponse->GetReasonPhrase()),
        // The response body may or may not have these fields
        ErrorCode(GetRawResponseField(RawResponse, "code")),
        Message(GetRawResponseField(RawResponse, "message"))
  {
    const auto& headers = RawResponse->GetHeaders();
    ClientRequestId = HttpShared::GetHeaderOrEmptyString(headers, HttpShared::MsClientRequestId);
    RequestId = HttpShared::GetHeaderOrEmptyString(headers, HttpShared::MsRequestId);
  }

  std::string RequestFailedException::GetRawResponseErrorMessage(
      std::unique_ptr<Azure::Core::Http::RawResponse> const& rawResponse)
  {
    std::string returnValue("Received an HTTP unsuccessful status code: ");
    // The status code will always be present in the rawResponse.
    returnValue += std::to_string(
        static_cast<typename std::underlying_type<Azure::Core::Http::HttpStatusCode>::type>(
            rawResponse->GetStatusCode()));

    // If there is a Reason phrase in the rawResponse, add it to the message.
    if (!rawResponse->GetReasonPhrase().empty())
    {
      returnValue += " Reason: " + rawResponse->GetReasonPhrase();
    }
    return returnValue;
  }

  std::string RequestFailedException::GetRawResponseField(
      std::unique_ptr<Azure::Core::Http::RawResponse> const& rawResponse,
      std::string fieldName)
  {
    auto& headers = rawResponse->GetHeaders();
    std::string contentType = HttpShared::GetHeaderOrEmptyString(headers, HttpShared::ContentType);
    std::vector<uint8_t> bodyBuffer = rawResponse->GetBody();
    std::string result;

    if (contentType.find("json") != std::string::npos)
    {
      auto jsonParser = Azure::Core::Json::_internal::json::parse(bodyBuffer);
      auto error = jsonParser.find("error");
      if (error != jsonParser.end() && error.value().contains(fieldName))
      {
        result = error.value()[fieldName].get<std::string>();
      }
    }

    return result;
  }
}} // namespace Azure::Core
