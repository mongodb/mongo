/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/crypto/CRC64.h>
#include <aws/core/utils/crypto/Factories.h>

using namespace Aws::Utils::Crypto;

CRC64::CRC64() : m_hashImpl(CreateCRC64Implementation()) {}

HashResult CRC64::Calculate(const Aws::String &str) {
  if (m_hashImpl != nullptr) {
    return m_hashImpl->Calculate(str);
  }
  return false;
}

HashResult CRC64::Calculate(Aws::IStream &stream) {
  if (m_hashImpl != nullptr) {
    return m_hashImpl->Calculate(stream);
  }
  return false;
}

void CRC64::Update(unsigned char *buffer, size_t bufferSize) {
  if (m_hashImpl != nullptr) {
    m_hashImpl->Update(buffer, bufferSize);
  }
}

HashResult CRC64::GetHash() {
  if (m_hashImpl != nullptr) {
    return m_hashImpl->GetHash();
  }
  return false;
}
