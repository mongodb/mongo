// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief BodyStream is used to read data to/from a service.
 */

#pragma once

#include "azure/core/platform.hpp"

#if defined(AZ_PLATFORM_POSIX)
#include <unistd.h>
#endif

#include "azure/core/context.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace Azure { namespace Core { namespace IO {

  /**
   * @brief Used to read data to/from a service.
   */
  class BodyStream {
  private:
    /**
     * @brief Read portion of data into a buffer.
     *
     * @remark This is the `OnRead` implementation that all derived classes need to provide.
     *
     * @param buffer Pointer to a byte buffer to read the data into.
     * @param count Size of the buffer to read the data into.
     * @param context A context to control the request lifetime.
     *
     * @return Number of bytes read.
     */
    virtual size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) = 0;

  public:
    /**
     * @brief Destructs `%BodyStream`.
     *
     */
    virtual ~BodyStream() = default;

    /**
     * @brief Get the length of the data.
     * @remark Used with the HTTP `Content-Length` header.
     */
    virtual int64_t Length() const = 0;

    /** @brief Resets the stream back to the beginning (for retries).
     * @remark Derived classes that send data in an HTTP request MUST override this and implement
     * it properly.
     */
    virtual void Rewind()
    {
      AZURE_ASSERT_MSG(
          false,
          "The specified BodyStream doesn't support Rewind which is required to guarantee fault "
          "tolerance when retrying any operation. Consider creating a MemoryBodyStream or "
          "FileBodyStream, which are rewindable.");
    }

    /**
     * @brief Read portion of data into a buffer.
     * @remark Throws if error/cancelled.
     *
     * @param buffer Pointer to a first byte of the byte buffer to read the data into.
     * @param count Size of the buffer to read the data into.
     * @param context A context to control the request lifetime.
     *
     * @return Number of bytes read.
     */
    size_t Read(
        uint8_t* buffer,
        size_t count,
        Azure::Core::Context const& context = Azure::Core::Context())
    {
      AZURE_ASSERT(buffer || count == 0);

      context.ThrowIfCancelled();
      return OnRead(buffer, count, context);
    }

    /**
     * @brief Read #Azure::Core::IO::BodyStream into a buffer until the buffer is filled, or until
     * the stream is read to end.
     *
     * @param buffer Pointer to a first byte of the byte buffer to read the data into.
     * @param count Size of the buffer to read the data into.
     * @param context A context to control the request lifetime.
     *
     * @return Number of bytes read.
     */
    size_t ReadToCount(
        uint8_t* buffer,
        size_t count,
        Azure::Core::Context const& context = Azure::Core::Context());

    /**
     * @brief Read #Azure::Core::IO::BodyStream until the stream is read to end, allocating memory
     * for the entirety of contents.
     *
     * @param context A context to control the request lifetime.
     *
     * @return A vector of bytes containing the entirety of data read from the \p body.
     */
    std::vector<uint8_t> ReadToEnd(Azure::Core::Context const& context = Azure::Core::Context());
  };

  /**
   * @brief #Azure::Core::IO::BodyStream providing data from an initialized memory buffer.
   */
  class MemoryBodyStream final : public BodyStream {
  private:
    const uint8_t* m_data;
    size_t m_length;
    size_t m_offset = 0;

    size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) override;

  public:
    // Forbid constructor for rval so we don't end up storing dangling ptr
    MemoryBodyStream(std::vector<uint8_t> const&&) = delete;

    /**
     * @brief Construct using vector of bytes.
     *
     * @param buffer Vector of bytes with the contents to provide the data from to the readers.
     */
    MemoryBodyStream(std::vector<uint8_t> const& buffer)
        : MemoryBodyStream(buffer.data(), buffer.size())
    {
    }

    /**
     * @brief Construct using buffer pointer and its size.
     *
     * @param data Pointer to a first byte of the buffer with the contents to provide the data
     * from to the readers.
     * @param length Size of the buffer.
     */
    explicit MemoryBodyStream(const uint8_t* data, size_t length) : m_data(data), m_length(length)
    {
      AZURE_ASSERT(data || length == 0);
    }

    int64_t Length() const override { return this->m_length; }

    /** @brief Rewind seeks the current stream to the start of the buffer. */
    void Rewind() override { m_offset = 0; }
  };

  namespace _internal {
    /**
     * @brief A concrete implementation of  #Azure::Core::IO::BodyStream used for reading data
     * from a file from any offset and length within it.
     */
    class RandomAccessFileBodyStream final : public BodyStream {
    private:
      // immutable
#if defined(AZ_PLATFORM_POSIX)
      int m_fileDescriptor;
#elif defined(AZ_PLATFORM_WINDOWS)
      void* m_filehandle;
#endif
      int64_t m_baseOffset;
      int64_t m_length;
      // mutable
      int64_t m_offset;

      size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) override;

    public:
#if defined(AZ_PLATFORM_POSIX)
      /**
       * @brief Construct from a file descriptor.
       *
       * @param fileDescriptor A file descriptor to an already opened file object that can be used
       * to identify the file.
       * @param offset The offset from the beginning of the file from which to start accessing the
       * data.
       * @param length The amounts of bytes, starting from the offset, that this stream can access
       * from the file.
       *
       * @remark The caller owns the file handle and needs to open it along with keeping it alive
       * for the necessary duration. The caller is also responsible for closing it once they are
       * done.
       */
      RandomAccessFileBodyStream(int fileDescriptor, int64_t offset, int64_t length)
          : m_fileDescriptor(fileDescriptor), m_baseOffset(offset), m_length(length), m_offset(0)
      {
        AZURE_ASSERT(fileDescriptor >= 0 && offset >= 0 && length >= 0);
      }

      RandomAccessFileBodyStream() : m_fileDescriptor(0), m_baseOffset(0), m_length(0), m_offset(0)
      {
      }

#elif defined(AZ_PLATFORM_WINDOWS)
      /**
       * @brief Construct from a file handle.
       *
       * @param fileHandle A file handle to an already opened file object that can be used to
       * identify the file.
       * @param offset The offset from the beginning of the file from which to start accessing the
       * data.
       * @param length The amounts of bytes, starting from the offset, that this stream can access
       * from the file.
       *
       * @remark The caller owns the file handle and needs to open it along with keeping it alive
       * for the necessary duration. The caller is also responsible for closing it once they are
       * done.
       */
      RandomAccessFileBodyStream(void* fileHandle, int64_t offset, int64_t length)
          : m_filehandle(fileHandle), m_baseOffset(offset), m_length(length), m_offset(0)
      {
        AZURE_ASSERT(fileHandle && offset >= 0 && length >= 0);
      }

      RandomAccessFileBodyStream() : m_filehandle(NULL), m_baseOffset(0), m_length(0), m_offset(0)
      {
      }
#endif

      // Rewind seeks back to 0
      void Rewind() override { this->m_offset = 0; }

      int64_t Length() const override { return this->m_length; }
    };

  } // namespace _internal

  /**
   * @brief A concrete implementation of #Azure::Core::IO::BodyStream used for reading data from a
   * file.
   */
  class FileBodyStream final : public BodyStream {
  private:
    // immutable
#if defined(AZ_PLATFORM_WINDOWS)
    void* m_filehandle;
#elif defined(AZ_PLATFORM_POSIX)
    int m_fileDescriptor;
#endif
    // mutable
    std::unique_ptr<_internal::RandomAccessFileBodyStream> m_randomAccessFileBodyStream;

    size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) override;

  public:
    /**
     * @brief Constructs `%FileBodyStream` from a file name.
     *
     * @param filename A reference to a file name string used to identify the file, which needs to
     * have the necessary file path specified to locate the file.
     *
     * @remark The #Azure::Core::IO::FileBodyStream owns the file object and is responsible for
     * opening and closing the file.
     *
     * @remark Do not write to the file while it is being used by the stream.
     */
    FileBodyStream(const std::string& filename);

    /**
     * @brief Closes the file and cleans up any resources.
     *
     */
    ~FileBodyStream();

    /** @brief Rewind seeks the current stream to the start of the file. */
    void Rewind() override;

    int64_t Length() const override;
  };

  /**
   * @brief A concrete implementation of #Azure::Core::IO::BodyStream that wraps another stream
   * and reports progress
   */
  class ProgressBodyStream : public BodyStream {
  private:
    BodyStream* m_bodyStream;
    int64_t m_bytesTransferred;
    std::function<void(int64_t bytesTransferred)> m_callback;

  private:
    size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) override;

  public:
    /**
     * @brief Constructs `%ProgressBodyStream` from a %BodyStream.
     *
     * @param bodyStream The body stream to wrap.
     *
     * @param callback The callback method used to report progress back to the caller.
     *
     * @remark The #Azure::Core::IO::ProgressBodyStream does not own the wrapped stream
     * and is not responsible for closing / cleaning up resources.
     */
    ProgressBodyStream(
        BodyStream& bodyStream,
        std::function<void(int64_t bytesTransferred)> callback);

    /** @brief Rewind seeks the current stream to the beginning. */
    void Rewind() override;

    int64_t Length() const override;
  };
}}} // namespace Azure::Core::IO
