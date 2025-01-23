/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/crypto/Hash.h>

namespace Aws {
namespace Utils {
namespace Stream {

static const size_t AWS_DATA_BUFFER_SIZE = 65536;

template <size_t DataBufferSize = AWS_DATA_BUFFER_SIZE>
class AwsChunkedStream {
 public:
  AwsChunkedStream(Http::HttpRequest *request, const std::shared_ptr<Aws::IOStream> &stream)
      : m_chunkingStream{Aws::MakeShared<StringStream>("AwsChunkedStream")}, m_request(request), m_stream(stream) {
    assert(m_stream != nullptr);
    if (m_stream == nullptr) {
      AWS_LOGSTREAM_ERROR("AwsChunkedStream", "stream is null");
    }
    assert(request != nullptr);
    if (request == nullptr) {
      AWS_LOGSTREAM_ERROR("AwsChunkedStream", "request is null");
    }
  }

  size_t BufferedRead(char *dst, size_t amountToRead) {
    assert(dst != nullptr);

    // only read and write to chunked stream if the underlying stream
    // is still in a valid state
    if (m_stream->good()) {
      // Try to read in a 64K chunk, if we cant we know the stream is over
      m_stream->read(m_data.GetUnderlyingData(), DataBufferSize);
      size_t bytesRead = static_cast<size_t>(m_stream->gcount());
      writeChunk(bytesRead);

      // if we've read everything from the stream, we want to add the trailer
      // to the underlying stream
      if ((m_stream->peek() == EOF || m_stream->eof()) && !m_stream->bad()) {
        writeTrailerToUnderlyingStream();
      }
    }

    // if the underlying stream is empty there is nothing to read
    if ((m_chunkingStream->peek() == EOF || m_chunkingStream->eof()) && !m_chunkingStream->bad()) {
      return 0;
    }

    // Read to destination buffer, return how much was read
    m_chunkingStream->read(dst, amountToRead);
    return static_cast<size_t>(m_chunkingStream->gcount());
  }

 private:
  void writeTrailerToUnderlyingStream() {
    Aws::StringStream chunkedTrailerStream;
    chunkedTrailerStream << "0\r\n";
    if (m_request->GetRequestHash().second != nullptr) {
      chunkedTrailerStream << "x-amz-checksum-" << m_request->GetRequestHash().first << ":"
                           << HashingUtils::Base64Encode(m_request->GetRequestHash().second->GetHash().GetResult()) << "\r\n";
    }
    chunkedTrailerStream << "\r\n";
    const auto chunkedTrailer = chunkedTrailerStream.str();
    if (m_chunkingStream->eof()) {
      m_chunkingStream->clear();
    }
    *m_chunkingStream << chunkedTrailer;
  }

  void writeChunk(size_t bytesRead) {
    if (m_request->GetRequestHash().second != nullptr) {
      m_request->GetRequestHash().second->Update(reinterpret_cast<unsigned char *>(m_data.GetUnderlyingData()), bytesRead);
    }

    if (m_chunkingStream != nullptr && !m_chunkingStream->bad()) {
      if (m_chunkingStream->eof()) {
        m_chunkingStream->clear();
      }
      *m_chunkingStream << Aws::Utils::StringUtils::ToHexString(bytesRead) << "\r\n";
      m_chunkingStream->write(m_data.GetUnderlyingData(), bytesRead);
      *m_chunkingStream << "\r\n";
    }
  }

  Aws::Utils::Array<char> m_data{DataBufferSize};
  std::shared_ptr<Aws::IOStream> m_chunkingStream;
  Http::HttpRequest *m_request{nullptr};
  std::shared_ptr<Aws::IOStream> m_stream;
};
}  // namespace Stream
}  // namespace Utils
}  // namespace Aws
