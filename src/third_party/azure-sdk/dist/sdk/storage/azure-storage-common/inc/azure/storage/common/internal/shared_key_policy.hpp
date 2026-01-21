// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/common/storage_credential.hpp"

#include <azure/core/http/policies/policy.hpp>

#include <memory>
#include <string>

namespace Azure { namespace Storage { namespace _internal {

  class SharedKeyPolicy final : public Core::Http::Policies::HttpPolicy {
  public:
    explicit SharedKeyPolicy(std::shared_ptr<StorageSharedKeyCredential> credential)
        : m_credential(std::move(credential))
    {
    }

    ~SharedKeyPolicy() override {}

    std::unique_ptr<HttpPolicy> Clone() const override
    {
      return std::make_unique<SharedKeyPolicy>(m_credential);
    }

    std::unique_ptr<Core::Http::RawResponse> Send(
        Core::Http::Request& request,
        Core::Http::Policies::NextHttpPolicy nextPolicy,
        Core::Context const& context) const override
    {
      request.SetHeader(
          "Authorization", "SharedKey " + m_credential->AccountName + ":" + GetSignature(request));
      return nextPolicy.Send(request, context);
    }

  private:
    std::string GetSignature(const Core::Http::Request& request) const;

    std::shared_ptr<StorageSharedKeyCredential> m_credential;
  };

}}} // namespace Azure::Storage::_internal
