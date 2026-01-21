// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Base type for all client option types, exposes various common client options like Retry
 * and Transport.
 */

#pragma once

#include "azure/core/http/http.hpp"
#include "azure/core/http/policies/policy.hpp"

#include <memory>
#include <vector>

namespace Azure { namespace Core { namespace _internal {

  /**
   * @brief  Base type for all client option types, exposes various common client options like Retry
   * and Transport.
   */
  struct ClientOptions
  {
    /**
     * @brief Destructor.
     *
     */
    virtual ~ClientOptions() = default;

    /**
     * @brief Define policies to be called one time for every HTTP request from an SDK client.
     *
     */
    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> PerOperationPolicies;

    /**
     * @brief Define policies to be called each time and SDK client tries to send the HTTP request.
     *
     */
    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> PerRetryPolicies;

    /**
     * @brief Move each policy from \p options into the new instance.
     *
     */
    ClientOptions(ClientOptions&&) = default;

    /**
     * @brief Copy each policy to the new instance.
     *
     */
    ClientOptions(ClientOptions const& other) { *this = other; }

    ClientOptions() = default;

    /**
     * @brief Move each policy from \p options into the this instance.
     *
     */
    ClientOptions& operator=(ClientOptions&&) = default;

    /**
     * @brief Copy each policy to the this instance.
     *
     */
    ClientOptions& operator=(const ClientOptions& other)
    {
      this->Retry = other.Retry;
      this->Transport = other.Transport;
      this->Telemetry = other.Telemetry;
      this->Log = other.Log;
      this->PerOperationPolicies.reserve(other.PerOperationPolicies.size());
      for (auto& policy : other.PerOperationPolicies)
      {
        this->PerOperationPolicies.emplace_back(policy->Clone());
      }
      this->PerRetryPolicies.reserve(other.PerRetryPolicies.size());
      for (auto& policy : other.PerRetryPolicies)
      {
        this->PerRetryPolicies.emplace_back(policy->Clone());
      }
      return *this;
    }

    /**
     * @brief Specify the number of retries and other retry-related options.
     *
     */
    Azure::Core::Http::Policies::RetryOptions Retry;

    /**
     * @brief Customized HTTP client. We're going to use the default one if this is empty.
     *
     */
    Azure::Core::Http::Policies::TransportOptions Transport;

    /**
     * @brief Telemetry options.
     *
     */
    Azure::Core::Http::Policies::TelemetryOptions Telemetry;

    /**
     * @brief Define the information to be used for logging.
     *
     */
    Azure::Core::Http::Policies::LogOptions Log;
  };

}}} // namespace Azure::Core::_internal
