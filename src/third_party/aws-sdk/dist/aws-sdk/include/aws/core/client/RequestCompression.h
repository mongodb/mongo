/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

using iostream_outcome =
    Aws::Utils::Outcome<std::shared_ptr<Aws::IOStream>, bool>;

namespace Aws {
namespace Client {
enum class CompressionAlgorithm { NONE, GZIP };

/**
 * Converts a compression Algorithms enum to String to be used as content-type
 * header value when compressing a request.
 * @param algorithm
 * @return string with HTTP content type algorithm id
 */
Aws::String AWS_CORE_API
GetCompressionAlgorithmId(const CompressionAlgorithm &algorithm);

/**
 * Request compression API
 */
class AWS_CORE_API RequestCompression final {
public:
  /**
   * Select the best matching algorithm based in proposed ones, config, length
   * of content and the available algorithms.
   * @param proposedAlgorithms
   * @param config
   * @param payloadLength
   * @return selected compression algorithm
   */
  CompressionAlgorithm
  selectAlgorithm(const Aws::Vector<CompressionAlgorithm> &proposedAlgorithms,
                  const Aws::Client::RequestCompressionConfig &config,
                  const size_t payloadLength);
  /**
   * Compress a IOStream input using the requested algorithm.
   * @param input
   * @param algorithm
   * @return IOStream compressed
   */
  iostream_outcome compress(std::shared_ptr<Aws::IOStream> input,
                            const CompressionAlgorithm &algorithm) const;
  /**
   * Uncompress a IOStream input using the requested algorithm.
   * @param input
   * @param algorithm
   * @return
   */
  iostream_outcome uncompress(std::shared_ptr<Aws::IOStream> input,
                              const CompressionAlgorithm &algorithm) const;
};
} // namespace Client
} // namespace Aws