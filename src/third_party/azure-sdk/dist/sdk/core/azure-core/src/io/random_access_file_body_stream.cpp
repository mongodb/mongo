// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/platform.hpp"

#if defined(AZ_PLATFORM_POSIX)
#include <errno.h>
#include <unistd.h>
#elif defined(AZ_PLATFORM_WINDOWS)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "azure/core/context.hpp"
#include "azure/core/io/body_stream.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

using Azure::Core::Context;
using namespace Azure::Core::IO::_internal;

size_t RandomAccessFileBodyStream::OnRead(
    uint8_t* buffer,
    size_t count,
    Azure::Core::Context const&)
{

#if defined(AZ_PLATFORM_POSIX)

  // Returning ssize_t from pread as a size_t is fine since we do a `< 0` check below and throw.
  auto numberOfBytesRead = pread(
      this->m_fileDescriptor,
      buffer,
      (std::min)(static_cast<int64_t>(count), this->m_length - this->m_offset),
      this->m_baseOffset + this->m_offset);

  if (numberOfBytesRead < 0)
  {
    throw std::runtime_error("Reading error. (Code Number: " + std::to_string(errno) + ")");
  }

#elif defined(AZ_PLATFORM_WINDOWS)

  DWORD numberOfBytesRead;
  auto o = OVERLAPPED();
  o.Offset = static_cast<DWORD>(this->m_baseOffset + this->m_offset);
  o.OffsetHigh = static_cast<DWORD>((this->m_baseOffset + this->m_offset) >> 32);

  HANDLE fileHandle = static_cast<HANDLE>(this->m_filehandle);

  auto result = ReadFile(
      fileHandle,
      buffer,
      // at most 4Gb to be read
      static_cast<DWORD>((std::min)(
          static_cast<uint64_t>(0xFFFFFFFFUL),
          static_cast<uint64_t>(
              (std::min)(static_cast<int64_t>(count), (this->m_length - this->m_offset))))),
      &numberOfBytesRead,
      &o);

  if (!result)
  {
    // Check error. If EOF, return bytes read to EOF.
    auto error = GetLastError();
    if (error != ERROR_HANDLE_EOF)
    {
      throw std::runtime_error("Reading error. (Code Number: " + std::to_string(error) + ")");
    }
  }
#endif

  this->m_offset += numberOfBytesRead;
  return numberOfBytesRead;
}
