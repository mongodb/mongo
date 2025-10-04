/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/crypto/CRC.h>
#include <aws/core/utils/crypto/Hash.h>
#include <aws/crt/checksum/CRC.h>

namespace Aws {
namespace Utils {
namespace Crypto {
/**
 * CRC64 hash implementation.
 */
class AWS_CORE_API CRC64 : public Hash {
public:
  CRC64();
  ~CRC64() override = default;
  HashResult Calculate(const Aws::String &str) override;
  HashResult Calculate(Aws::IStream &stream) override;
  void Update(unsigned char *buffer, size_t bufferSize) override;
  HashResult GetHash() override;

private:
  std::shared_ptr<Hash> m_hashImpl;
};

using CRC64Impl = CRCChecksum<uint64_t, Aws::Crt::Checksum::ComputeCRC64NVME,
                              ConvertToBuffer<uint64_t>>;
} // namespace Crypto
} // namespace Utils
} // namespace Aws
