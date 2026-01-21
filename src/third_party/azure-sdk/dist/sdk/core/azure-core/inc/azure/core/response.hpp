// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Wraps the raw HTTP response from a request made to the service into a response of a
 * specific type.
 */

#pragma once

#include "azure/core/http/http.hpp"
#include "azure/core/nullable.hpp"

#include <memory> // for unique_ptr
#include <stdexcept>
#include <utility> // for move

namespace Azure {
/**
 * @brief Represents the result of an Azure operation over HTTP by wrapping the raw HTTP response
 * from a request made to the service into a response of a specific type.
 *
 * @tparam T A specific type of value to get from the raw HTTP response.
 */
template <class T> class Response final {

public:
  /// The value returned by the service.
  T Value;
  /// The HTTP response returned by the service.
  std::unique_ptr<Azure::Core::Http::RawResponse> RawResponse;

  /**
   * @brief Constructs a `%Response` with the value and raw response returned by the service.
   *
   * @param value The value returned by the service.
   * @param rawResponse The HTTP response returned by the service.
   */
  explicit Response(T value, std::unique_ptr<Azure::Core::Http::RawResponse> rawResponse)
      : Value(std::move(value)), RawResponse(std::move(rawResponse))
  {
  }
};

} // namespace Azure
