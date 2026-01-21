// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/common/internal/storage_per_retry_policy.hpp"

#include "azure/storage/common/internal/constants.hpp"
#include "azure/storage/common/internal/reliable_stream.hpp"

#include <azure/core/datetime.hpp>
#include <azure/core/platform.hpp>

#include <algorithm>
#include <chrono>

namespace Azure { namespace Storage { namespace _internal {

  std::unique_ptr<Core::Http::RawResponse> StoragePerRetryPolicy::Send(
      Core::Http::Request& request,
      Core::Http::Policies::NextHttpPolicy nextPolicy,
      Core::Context const& context) const
  {
    const auto& headers = request.GetHeaders();
    if (headers.find(HttpHeaderDate) == headers.end())
    {
      // add x-ms-date header in RFC1123 format
      request.SetHeader(
          HttpHeaderXMsDate,
          DateTime(std::chrono::system_clock::now())
              .ToString(Azure::DateTime::DateFormat::Rfc1123));
    }

    auto cancelTimepoint = context.GetDeadline();
    if (cancelTimepoint == (Azure::DateTime::max)())
    {
      request.GetUrl().RemoveQueryParameter(HttpQueryTimeout);
    }
    else
    {
      auto currentTimepoint = DateTime(std::chrono::system_clock::now());
      int64_t numSeconds = (cancelTimepoint > currentTimepoint)
          ? std::chrono::duration_cast<std::chrono::seconds>(cancelTimepoint - currentTimepoint)
                .count()
          : -1;
      request.GetUrl().AppendQueryParameter(
          HttpQueryTimeout, std::to_string((std::max)(numSeconds, int64_t(1))));
    }

    std::string client_request_id;
    if (context.TryGetValue(ReliableStreamClientRequestIdKey, client_request_id))
    {
      if (!client_request_id.empty())
      {
        request.SetHeader(HttpHeaderClientRequestId, client_request_id);
      }
    }

    return nextPolicy.Send(request, context);
  }

}}} // namespace Azure::Storage::_internal
