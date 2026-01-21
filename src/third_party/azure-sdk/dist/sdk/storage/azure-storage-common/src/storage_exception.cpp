// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/common/storage_exception.hpp"

#include "azure/storage/common/internal/constants.hpp"
#include "azure/storage/common/internal/xml_wrapper.hpp"

#include <azure/core/http/policies/policy.hpp>
#include <azure/core/internal/json/json.hpp>

#include <type_traits>

namespace Azure { namespace Storage {
  StorageException StorageException::CreateFromResponse(
      std::unique_ptr<Azure::Core::Http::RawResponse> response)
  {
    std::vector<uint8_t> bodyBuffer = std::move(response->GetBody());

    auto httpStatusCode = response->GetStatusCode();
    std::string reasonPhrase = response->GetReasonPhrase();
    std::string requestId;
    if (response->GetHeaders().find(_internal::HttpHeaderRequestId) != response->GetHeaders().end())
    {
      requestId = response->GetHeaders().at(_internal::HttpHeaderRequestId);
    }

    std::string clientRequestId;
    if (response->GetHeaders().find(_internal::HttpHeaderClientRequestId)
        != response->GetHeaders().end())
    {
      clientRequestId = response->GetHeaders().at(_internal::HttpHeaderClientRequestId);
    }

    std::string errorCode;
    std::string message;
    std::map<std::string, std::string> additionalInformation;

    if (response->GetHeaders().find(_internal::HttpHeaderContentType)
        != response->GetHeaders().end())
    {
      if (response->GetHeaders().at(_internal::HttpHeaderContentType).find("xml")
          != std::string::npos)
      {
        auto xmlReader = _internal::XmlReader(
            reinterpret_cast<const char*>(bodyBuffer.data()), bodyBuffer.size());

        enum class XmlTagName
        {
          XmlTagError,
          XmlTagCode,
          XmlTagMessage,
          XmlTagUnknown,
        };
        std::vector<XmlTagName> path;
        std::string startTagName;

        while (true)
        {
          auto node = xmlReader.Read();
          if (node.Type == _internal::XmlNodeType::End)
          {
            break;
          }
          else if (node.Type == _internal::XmlNodeType::EndTag)
          {
            startTagName.clear();
            if (path.size() > 0)
            {
              path.pop_back();
            }
            else
            {
              break;
            }
          }
          else if (node.Type == _internal::XmlNodeType::StartTag)
          {
            startTagName = node.Name;
            if (node.Name == "Error")
            {
              path.emplace_back(XmlTagName::XmlTagError);
            }
            else if (node.Name == "Code")
            {
              path.emplace_back(XmlTagName::XmlTagCode);
            }
            else if (node.Name == "Message")
            {
              path.emplace_back(XmlTagName::XmlTagMessage);
            }
            else
            {
              path.emplace_back(XmlTagName::XmlTagUnknown);
            }
          }
          else if (node.Type == _internal::XmlNodeType::Text)
          {
            if (path.size() == 2 && path[0] == XmlTagName::XmlTagError
                && path[1] == XmlTagName::XmlTagCode)
            {
              errorCode = node.Value;
            }
            else if (
                path.size() == 2 && path[0] == XmlTagName::XmlTagError
                && path[1] == XmlTagName::XmlTagMessage)
            {
              message = node.Value;
            }
            else if (
                path.size() == 2 && path[0] == XmlTagName::XmlTagError
                && path[1] == XmlTagName::XmlTagUnknown)
            {
              if (!startTagName.empty())
              {
                additionalInformation.emplace(std::move(startTagName), node.Value);
              }
            }
          }
        }
      }
      else if (
          response->GetHeaders().at(_internal::HttpHeaderContentType).find("html")
          != std::string::npos)
      {
        // TODO: add a refined message parsed from result.
        message = std::string(bodyBuffer.begin(), bodyBuffer.end());
      }
      else if (
          response->GetHeaders().at(_internal::HttpHeaderContentType).find("json")
          != std::string::npos)
      {
        auto jsonParser = Azure::Core::Json::_internal::json::parse(bodyBuffer);
        errorCode = jsonParser["error"]["code"].get<std::string>();
        message = jsonParser["error"]["message"].get<std::string>();
      }
      else
      {
        // TODO: add a refined message parsed from result.
        message = std::string(bodyBuffer.begin(), bodyBuffer.end());
      }
    }

    if (errorCode.empty()
        && response->GetHeaders().find("x-ms-error-code") != response->GetHeaders().end())
    {
      errorCode = response->GetHeaders().at("x-ms-error-code");
    }

    StorageException result = StorageException(
        std::to_string(static_cast<std::underlying_type<Azure::Core::Http::HttpStatusCode>::type>(
            httpStatusCode))
        + " " + reasonPhrase + "\n" + message + "\nRequest ID: " + requestId);
    result.StatusCode = httpStatusCode;
    result.ReasonPhrase = std::move(reasonPhrase);
    result.RequestId = std::move(requestId);
    result.ClientRequestId = std::move(clientRequestId);
    result.ErrorCode = std::move(errorCode);
    result.Message = std::move(message);
    result.RawResponse = std::move(response);
    result.AdditionalInformation = std::move(additionalInformation);
    return result;
  }
}} // namespace Azure::Storage
