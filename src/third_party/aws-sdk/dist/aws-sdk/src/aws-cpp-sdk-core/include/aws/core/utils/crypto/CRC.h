/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/crypto/Hash.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <type_traits>

namespace Aws {
namespace Utils {
namespace Crypto {
template <typename HashT> ByteBuffer ConvertToBuffer(HashT value) {
  static_assert(std::is_integral<HashT>::value,
                "Must use integral type to convert to buffer");
  Aws::Utils::ByteBuffer buffer(sizeof(HashT));
  for (size_t i = 0; i < sizeof(HashT); ++i) {
    size_t shiftSize = (sizeof(HashT) - 1) * 8 - i * 8;
    buffer[i] = (value >> shiftSize) & 0xFF;
  }
  return buffer;
}

template <typename RunningChecksumT,
          RunningChecksumT (*CRTChecksumFuncT)(Crt::ByteCursor,
                                               RunningChecksumT),
          ByteBuffer (*ByteBufferFuncT)(RunningChecksumT)>
class CRCChecksum : public Hash {
public:
  CRCChecksum() : m_runningChecksum{0} {}

  ~CRCChecksum() override = default;

  HashResult Calculate(const Aws::String &str) override {
    Aws::Crt::ByteCursor byteCursor = Aws::Crt::ByteCursorFromArray(
        reinterpret_cast<const uint8_t *>(str.data()), str.size());
    m_runningChecksum = CRTChecksumFuncT(byteCursor, m_runningChecksum);
    return ByteBufferFuncT(m_runningChecksum);
  };

  HashResult Calculate(Aws::IStream &stream) override {
    auto currentPos = stream.tellg();
    if (stream.eof()) {
      currentPos = 0;
      stream.clear();
    }

    stream.seekg(0, Aws::IStream::beg);

    uint8_t streamBuffer
        [Aws::Utils::Crypto::Hash::INTERNAL_HASH_STREAM_BUFFER_SIZE];
    while (stream.good()) {
      stream.read(reinterpret_cast<char *>(streamBuffer),
                  Aws::Utils::Crypto::Hash::INTERNAL_HASH_STREAM_BUFFER_SIZE);
      const auto bytesRead = static_cast<size_t>(stream.gcount());

      if (bytesRead > 0) {
        Aws::Crt::ByteCursor byteCursor =
            Aws::Crt::ByteCursorFromArray(streamBuffer, bytesRead);
        m_runningChecksum = CRTChecksumFuncT(byteCursor, m_runningChecksum);
      }
    }

    if (stream.bad()) {
      AWS_LOGSTREAM_ERROR(
          "CRCChecksum",
          "Stream encountered an error while calculating CRC Checksum");
    }

    stream.clear();
    stream.seekg(currentPos, Aws::IStream::beg);

    return ByteBufferFuncT(m_runningChecksum);
  };

  void Update(unsigned char *buffer, size_t bufferSize) override {
    Aws::Crt::ByteCursor byteCursor =
        Aws::Crt::ByteCursorFromArray(buffer, bufferSize);
    m_runningChecksum = CRTChecksumFuncT(byteCursor, m_runningChecksum);
  };

  HashResult GetHash() override { return ByteBufferFuncT(m_runningChecksum); };

private:
  RunningChecksumT m_runningChecksum;
};
} // namespace Crypto
} // namespace Utils
} // namespace Aws
