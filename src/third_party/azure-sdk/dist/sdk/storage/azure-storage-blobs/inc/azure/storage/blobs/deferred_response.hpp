// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <azure/core/response.hpp>

#include <functional>

namespace Azure { namespace Storage {
  namespace Blobs {
    class BlobServiceBatch;
    class BlobContainerBatch;
  } // namespace Blobs
  /**
   * @brief Base type for a deferred response.
   */
  template <typename T> class DeferredResponse final {
  public:
    DeferredResponse(const DeferredResponse&) = delete;
    /** @brief Construct a new deferred response, moving from an existing DeferredResponse */
    DeferredResponse(DeferredResponse&&) = default;
    DeferredResponse& operator=(const DeferredResponse&) = delete;
    /** Move a DeferredResponse to another DeferredResponse object.. */
    DeferredResponse& operator=(DeferredResponse&&) = default;

    /**
     * @brief Gets the deferred response.
     *
     * @remark It's undefined behavior to call this function before the response or exception is
     * available.
     *
     * @return The deferred response. An exception is thrown if error occurred.
     */
    Response<T> GetResponse() const { return m_func(); }

  private:
    DeferredResponse(std::function<Response<T>()> func) : m_func(std::move(func)) {}

  private:
    std::function<Response<T>()> m_func;

    friend class Blobs::BlobServiceBatch;
    friend class Blobs::BlobContainerBatch;
  };
}} // namespace Azure::Storage
