// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Define RequestFailedException. It is used by HTTP exceptions.
 */

#pragma once

#include "azure/core/http/http_status_code.hpp"
#include "azure/core/http/raw_response.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Azure { namespace Core {
  /**
   * @brief An error while trying to send a request to Azure service.
   *
   * @details
   * A RequestFailedException is sometimes generated as a result of an HTTP response returned from
   * the service, and is sometimes generated for other reasons. The contents of the
   * RequestFailedException depend on whether the exception was thrown as a result of an HTTP
   * response error or other reasons.
   *
   * To determine which form of RequestFailedException has occurred, a client can check the
   * `RequestFailedException::RawResponse` field - if that value is null, then the request failed
   * for some reason other than an HTTP response, the reason can be determined by calling
   * `RequestFailedException::what()`.
   *
   * If the request has failed due to an HTTP response code, the client can inspect other fields in
   * the exception to determine the actual failure returned by the service.
   *
   * Most Azure services follow the [Azure standards for error condition responses]
   * (https://github.com/microsoft/api-guidelines/blob/vNext/Guidelines.md#7102-error-condition-responses)
   * and return an `error` object containing two properties, `code` and `message`. These properties
   * are used to populate the RequestFailedException::ErrorCode and RequestFailedException::Message
   * fields
   *
   * \code{.cpp}
   *   catch (Azure::Core::RequestFailedException const& e)
   *   {
   *     std::cout << "Request Failed Exception happened:" << std::endl << e.what() << std::endl;
   *     if (e.RawResponse)
   *     {
   *       std::cout << "Error Code: " << e.ErrorCode << std::endl;
   *       std::cout << "Error Message: " << e.Message << std::endl;
   *     }
   *     return 0;
   * }
   *
   * \endcode
   */
  class RequestFailedException : public std::runtime_error {
  public:
    /**
     * @brief The entire HTTP raw response.
     *
     */
    std::unique_ptr<Azure::Core::Http::RawResponse> RawResponse;

    /**
     * @brief The HTTP response code.
     *
     */
    Azure::Core::Http::HttpStatusCode StatusCode = Azure::Core::Http::HttpStatusCode::None;

    /**
     * @brief The HTTP reason phrase from the response.
     *
     */
    std::string ReasonPhrase;

    /**
     * @brief The client request header (`x-ms-client-request-id`) from the HTTP response.
     *
     */
    std::string ClientRequestId;

    /**
     * @brief The request ID header (`x-ms-request-id`) from the HTTP response.
     *
     */
    std::string RequestId;

    /**
     * @brief The error code from service returned in the HTTP response.
     *
     * For more information, see [Azure standards for error condition responses]
     * (https://github.com/microsoft/api-guidelines/blob/vNext/Guidelines.md#7102-error-condition-responses),
     * specifically the handling of the "code" property.
     *
     * Note that the contents of the `ErrorCode` is service dependent.
     *
     */
    std::string ErrorCode;

    /**
     * @brief The error message from the service returned in the HTTP response.
     *
     * For more information, see [Azure standards for error condition responses]
     * (https://github.com/microsoft/api-guidelines/blob/vNext/Guidelines.md#7102-error-condition-responses),
     * specifically the handling of the "message" property.
     *
     * @note This string is purely for informational or diagnostic purposes, and should't be relied
     * on at runtime.
     *
     */
    std::string Message;

    /**
     * @brief Constructs a new `%RequestFailedException` with a \p message string.
     *
     * @note An Exception without an HTTP raw response represents an exception that is not
     * associated with an HTTP response. Typically this is an error which occurred before the
     * response was received from the service.
     *
     * @param what The explanatory string.
     */
    explicit RequestFailedException(std::string const& what);

    /**
     * @brief Constructs a new `%RequestFailedException` object with an HTTP raw response.
     *
     * @note The HTTP raw response is parsed to populate [information expected from all Azure
     * Services](https://github.com/microsoft/api-guidelines/blob/vNext/Guidelines.md#7102-error-condition-responses)
     * like the status code, reason phrase and some headers like the request ID. A concrete Service
     * exception which derives from this exception uses its constructor to parse the HTTP raw
     * response adding the service specific values to the exception.
     *
     * @param rawResponse The HTTP raw response from the service.
     */
    explicit RequestFailedException(std::unique_ptr<Azure::Core::Http::RawResponse>& rawResponse);

    /**
     * @brief Constructs a new `%RequestFailedException` by copying from an existing one.
     * @note Copies the #Azure::Core::Http::RawResponse into the new `RequestFailedException`.
     *
     * @param other The `%RequestFailedException` to be copied.
     */
    RequestFailedException(const RequestFailedException& other)
        : std::runtime_error(other.Message),
          RawResponse(
              other.RawResponse
                  ? std::make_unique<Azure::Core::Http::RawResponse>(*other.RawResponse)
                  : nullptr),
          StatusCode(other.StatusCode), ReasonPhrase(other.ReasonPhrase),
          ClientRequestId(other.ClientRequestId), RequestId(other.RequestId),
          ErrorCode(other.ErrorCode), Message(other.Message)
    {
    }

    /**
     * @brief Constructs a new `%RequestFailedException` by moving in an existing one.
     * @param other The `%RequestFailedException` to move in.
     */
    RequestFailedException(RequestFailedException&& other) = default;

    /**
     * @brief An instance of `%RequestFailedException` class cannot be assigned.
     *
     */
    RequestFailedException& operator=(const RequestFailedException&) = delete;

    /**
     * @brief An instance of `%RequestFailedException` class cannot be moved into another instance
     * after creation.
     *
     */
    RequestFailedException& operator=(RequestFailedException&&) = delete;

    /**
     * @brief Destructs `%RequestFailedException`.
     *
     */
    ~RequestFailedException() = default;

  private:
    static std::string GetRawResponseField(
        std::unique_ptr<Azure::Core::Http::RawResponse> const& rawResponse,
        std::string fieldName);

    /**
     * @brief Returns a descriptive string for this RawResponse.
     */
    static std::string GetRawResponseErrorMessage(
        std::unique_ptr<Azure::Core::Http::RawResponse> const& rawResponse);
  };
}} // namespace Azure::Core
