// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Client options for #Azure::Core::Credentials::TokenCredential.
 */

#pragma once

#include "azure/core/internal/client_options.hpp"

namespace Azure { namespace Core { namespace Credentials {
  /**
   * @brief Client options for #Azure::Core::Credentials::TokenCredential.
   *
   */
  struct TokenCredentialOptions : public Azure::Core::_internal::ClientOptions
  {
  };
}}} // namespace Azure::Core::Credentials
