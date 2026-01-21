// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <azure/core/exception.hpp>
#include <azure/core/http/http.hpp>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace Azure { namespace Storage {

  /**
   * @brief An exception thrown when storage service request fails.
   */
  struct StorageException final : public Azure::Core::RequestFailedException
  {
    /**
     * @brief Constructs a #StorageException with a message.
     *
     * @param what The explanatory string.
     */
    explicit StorageException(const std::string& what) : RequestFailedException(what) {}

    /**
     * Some storage-specific information in response body.
     */
    std::map<std::string, std::string> AdditionalInformation;

    /**
     * @brief Constructs a #StorageException from a failed storage service response.
     *
     * @param response Raw HTTP response from storage service.
     * @return #StorageException.
     */
    static StorageException CreateFromResponse(
        std::unique_ptr<Azure::Core::Http::RawResponse> response);
  };
}} // namespace Azure::Storage
