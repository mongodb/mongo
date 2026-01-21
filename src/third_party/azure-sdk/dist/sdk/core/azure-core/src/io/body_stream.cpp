// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/platform.hpp"

#if defined(AZ_PLATFORM_POSIX)
#include <errno.h>
#include <fcntl.h> // for open and _O_RDONLY
#include <unistd.h> // for lseek

#include <sys/types.h> // for lseek
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
#include "azure/core/internal/io/null_body_stream.hpp"
#include "azure/core/io/body_stream.hpp"

#include <algorithm>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using Azure::Core::Context;
using namespace Azure::Core::IO;

#if defined(AZ_PLATFORM_WINDOWS)
// For an abundance of caution, adding this as a compile time check since we are using static_cast
// between windows HANDLE and void* to avoid having windows.h headers exposed in public headers.
static_assert(sizeof(void*) >= sizeof(HANDLE), "We must be able to cast HANDLE to void* and back.");
#endif

// Keep reading until buffer is all fill out of the end of stream content is reached
size_t BodyStream::ReadToCount(uint8_t* buffer, size_t count, Context const& context)
{
  AZURE_ASSERT(buffer || count == 0);

  size_t totalRead = 0;

  for (;;)
  {
    size_t readBytes = this->Read(buffer + totalRead, count - totalRead, context);
    totalRead += readBytes;
    // Reach all of buffer size
    if (totalRead == count || readBytes == 0)
    {
      return totalRead;
    }
  }
}

std::vector<uint8_t> BodyStream::ReadToEnd(Context const& context)
{
  constexpr size_t chunkSize = 1024 * 8;
  auto buffer = std::vector<uint8_t>();

  for (auto chunkNumber = 0;; chunkNumber++)
  {
    buffer.resize((static_cast<decltype(buffer)::size_type>(chunkNumber) + 1) * chunkSize);
    size_t readBytes
        = this->ReadToCount(buffer.data() + (chunkNumber * chunkSize), chunkSize, context);

    if (readBytes < chunkSize)
    {
      buffer.resize(static_cast<size_t>((chunkNumber * chunkSize) + readBytes));
      return buffer;
    }
  }
}

size_t MemoryBodyStream::OnRead(uint8_t* buffer, size_t count, Context const& context)
{
  (void)context;
  size_t copy_length = (std::min)(count, this->m_length - this->m_offset);
  // Copy what's left or just the count
  std::memcpy(buffer, this->m_data + m_offset, static_cast<size_t>(copy_length));
  // move position
  m_offset += copy_length;

  return copy_length;
}

FileBodyStream::FileBodyStream(const std::string& filename)
{
  AZURE_ASSERT_MSG(filename.size() > 0, "The file name must not be an empty string.");

#if defined(AZ_PLATFORM_WINDOWS)
  HANDLE fileHandle = INVALID_HANDLE_VALUE;
  try
  {
#if !defined(WINAPI_PARTITION_DESKTOP) \
    || WINAPI_PARTITION_DESKTOP // See azure/core/platform.hpp for explanation.
    fileHandle = CreateFile(
        filename.data(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, // Using this as an optimization since we know file access is
                                   // intended to be sequential from beginning to end.
        NULL);
#else
    fileHandle = CreateFile2(
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().from_bytes(filename).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        OPEN_EXISTING,
        NULL);
#endif

    if (fileHandle == INVALID_HANDLE_VALUE)
    {
      throw std::runtime_error("Failed to open file for reading. File name: '" + filename + "'");
    }
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(fileHandle, &fileSize))
    {
      throw std::runtime_error("Failed to get size of file. File name: '" + filename + "'");
    }
    m_filehandle = static_cast<void*>(fileHandle);
    m_randomAccessFileBodyStream = std::make_unique<_internal::RandomAccessFileBodyStream>(
        _internal::RandomAccessFileBodyStream(m_filehandle, 0, fileSize.QuadPart));
  }
  catch (...)
  {
    if (fileHandle != INVALID_HANDLE_VALUE)
    {
      CloseHandle(fileHandle);
    }
    throw;
  }

#elif defined(AZ_PLATFORM_POSIX)

  try
  {
    m_fileDescriptor = open(filename.data(), O_RDONLY);
    if (m_fileDescriptor == -1)
    {
      throw std::runtime_error("Failed to open file for reading. File name: '" + filename + "'");
    }
    int64_t fileSize = lseek(m_fileDescriptor, 0, SEEK_END);
    if (fileSize == -1)
    {
      throw std::runtime_error("Failed to get size of file. File name: '" + filename + "'");
    }
    m_randomAccessFileBodyStream = std::make_unique<_internal::RandomAccessFileBodyStream>(
        _internal::RandomAccessFileBodyStream(m_fileDescriptor, 0, fileSize));
  }
  catch (...)
  {
    close(m_fileDescriptor);
    throw;
  }

#endif
}

FileBodyStream::~FileBodyStream()
{
#if defined(AZ_PLATFORM_WINDOWS)
  if (m_filehandle)
  {
    CloseHandle(static_cast<HANDLE>(m_filehandle));
    m_filehandle = NULL;
  }
#elif defined(AZ_PLATFORM_POSIX)
  if (m_fileDescriptor)
  {
    close(m_fileDescriptor);
    m_fileDescriptor = 0;
  }
#endif
}

size_t FileBodyStream::OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context)
{
  return m_randomAccessFileBodyStream->Read(buffer, count, context);
}

void FileBodyStream::Rewind() { m_randomAccessFileBodyStream->Rewind(); }

int64_t FileBodyStream::Length() const { return m_randomAccessFileBodyStream->Length(); }

ProgressBodyStream::ProgressBodyStream(
    BodyStream& bodyStream,
    std::function<void(int64_t bytesTransferred)> callback)
    : m_bodyStream(&bodyStream), m_bytesTransferred(0), m_callback(std::move(callback))
{
}

void ProgressBodyStream::Rewind()
{
  m_bodyStream->Rewind();
  m_bytesTransferred = 0;
  m_callback(m_bytesTransferred);
}

size_t ProgressBodyStream::OnRead(
    uint8_t* buffer,
    size_t count,
    Azure::Core::Context const& context)
{
  size_t read = m_bodyStream->Read(buffer, count, context);
  m_bytesTransferred += read;
  m_callback(m_bytesTransferred);

  return read;
}

int64_t ProgressBodyStream::Length() const { return m_bodyStream->Length(); }

using Azure::Core::IO::_internal::NullBodyStream;

NullBodyStream* NullBodyStream::GetNullBodyStream()
{
  static NullBodyStream nullBodyStream;
  return &nullBodyStream;
}
