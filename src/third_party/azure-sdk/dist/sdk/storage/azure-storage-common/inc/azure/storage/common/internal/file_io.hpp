// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <azure/core/platform.hpp>

#include <cstdint>
#include <string>

namespace Azure { namespace Storage { namespace _internal {

#if defined(AZ_PLATFORM_WINDOWS)
  using FileHandle = void*;
#elif defined(AZ_PLATFORM_POSIX)
  using FileHandle = int;
#endif

  class FileReader final {
  public:
    FileReader(const std::string& filename);

    ~FileReader();

    FileHandle GetHandle() const { return m_handle; }

    int64_t GetFileSize() const { return m_fileSize; }

  private:
    FileHandle m_handle;
    int64_t m_fileSize;
  };

  class FileWriter final {
  public:
    FileWriter(const std::string& filename);

    ~FileWriter();

    FileHandle GetHandle() const { return m_handle; }

    void Write(const uint8_t* buffer, size_t length, int64_t offset);

  private:
    FileHandle m_handle;
  };

}}} // namespace Azure::Storage::_internal
