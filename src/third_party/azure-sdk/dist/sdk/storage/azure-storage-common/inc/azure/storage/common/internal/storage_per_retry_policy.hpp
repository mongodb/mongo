// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <azure/core/http/policies/policy.hpp>

#include <memory>

namespace Azure { namespace Storage { namespace _internal {

  class StoragePerRetryPolicy final : public Core::Http::Policies::HttpPolicy {
  public:
    ~StoragePerRetryPolicy() override {}

    std::unique_ptr<HttpPolicy> Clone() const override
    {
      return std::make_unique<StoragePerRetryPolicy>(*this);
    }

    std::unique_ptr<Core::Http::RawResponse> Send(
        Core::Http::Request& request,
        Core::Http::Policies::NextHttpPolicy nextPolicy,
        Core::Context const& context) const override;
  };

}}} // namespace Azure::Storage::_internal
