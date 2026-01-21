// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/common/dll_import_export.hpp"

#include <azure/core/context.hpp>
#include <azure/core/io/body_stream.hpp>

#include <algorithm>
#include <functional>
#include <memory>

namespace Azure { namespace Storage { namespace _internal {

  AZ_STORAGE_COMMON_DLLEXPORT extern const Azure::Core::Context::Key
      ReliableStreamClientRequestIdKey;

  // Options used by reliable stream
  struct ReliableStreamOptions final
  {
    // configures the maximun retries to be done.
    int32_t MaxRetryRequests;
  };

  /**
   * @brief Decorates a body stream by providing reliability while readind from it.
   * ReliableStream uses an HTTPGetter callback (provided on constructor) to get a bodyStream
   * starting on last known offset to resume a fail Read() operation.
   *
   * @remark An HTTPGetter callback is expected to verify the initial `eTag` from first HTTP request
   * to ensure read operation will continue on the same content.
   *
   * @remark An HTTPGetter callback is expected to calculate and set the range header based on the
   * offset provided by the ReliableStream.
   *
   */
  class ReliableStream final : public Azure::Core::IO::BodyStream {
  private:
    // initial bodyStream.
    std::unique_ptr<Azure::Core::IO::BodyStream> m_inner;
    // Configuration for the re-triable stream
    ReliableStreamOptions const m_options;
    // callback to get a bodyStream in case Read operation fails
    std::function<
        std::unique_ptr<Azure::Core::IO::BodyStream>(int64_t, Azure::Core::Context const&)>
        m_streamReconnector;
    // Options to use when getting a new bodyStream like current offset
    int64_t m_retryOffset;

    size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) override;

  public:
    explicit ReliableStream(
        std::unique_ptr<Azure::Core::IO::BodyStream> inner,
        ReliableStreamOptions const options,
        std::function<
            std::unique_ptr<Azure::Core::IO::BodyStream>(int64_t, Azure::Core::Context const&)>
            streamReconnector)
        : m_inner(std::move(inner)), m_options(options),
          m_streamReconnector(std::move(streamReconnector)), m_retryOffset(0)
    {
    }

    int64_t Length() const override { return this->m_inner->Length(); }
    void Rewind() override
    {
      // Rewind directly from a transportAdapter body stream (like libcurl) would throw
      this->m_inner->Rewind();
      this->m_retryOffset = 0;
    }
  };

}}} // namespace Azure::Storage::_internal
