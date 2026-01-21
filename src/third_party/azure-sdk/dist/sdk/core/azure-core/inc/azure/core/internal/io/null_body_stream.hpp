// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief A null body stream for HTTP requests without a payload.
 */

#pragma once

#include "azure/core/io/body_stream.hpp"

namespace Azure { namespace Core { namespace IO { namespace _internal {

  /**
   * @brief Empty #Azure::Core::IO::BodyStream.
   * @remark Used for requests with no body.
   */
  class NullBodyStream final : public BodyStream {
  private:
    size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) override
    {
      (void)context;
      (void)buffer;
      (void)count;
      return 0;
    }

  public:
    /// Constructor.
    explicit NullBodyStream() {}

    int64_t Length() const override { return 0; }

    void Rewind() override {}

    /**
     * @brief Gets a singleton instance of a #Azure::Core::IO::_internal::NullBodyStream.
     *
     */
    static NullBodyStream* GetNullBodyStream();
  };

}}}} // namespace Azure::Core::IO::_internal
