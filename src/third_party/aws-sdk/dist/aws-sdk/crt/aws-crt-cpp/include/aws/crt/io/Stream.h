#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>
#include <aws/crt/RefCounted.h>
#include <aws/crt/Types.h>
#include <aws/io/stream.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            using StreamStatus = aws_stream_status;

            /**
             * @deprecated Use int64_t instead for offsets in public APIs.
             */
            using OffsetType = aws_off_t;

            /**
             * Controls the direction to seek from
             */
            enum class StreamSeekBasis
            {
                Begin = AWS_SSB_BEGIN,
                End = AWS_SSB_END,
            };

            /***
             * Interface for building an Object oriented stream that will be honored by the CRT's low-level
             * aws_input_stream interface. To use, create a subclass of InputStream and define the abstract
             * functions.
             */
            class AWS_CRT_CPP_API InputStream : public std::enable_shared_from_this<InputStream>,
                                                public RefCounted<InputStream>
            {
              public:
                virtual ~InputStream();

                InputStream(const InputStream &) = delete;
                InputStream &operator=(const InputStream &) = delete;
                InputStream(InputStream &&) = delete;
                InputStream &operator=(InputStream &&) = delete;

                explicit operator bool() const noexcept { return IsValid(); }

                /**
                 * @return true/false if this object is in a valid state
                 */
                virtual bool IsValid() const noexcept = 0;

                /// @private
                aws_input_stream *GetUnderlyingStream() noexcept { return &m_underlying_stream; }

                /**
                 * Reads data from the stream into a buffer
                 * @param dest buffer to add the read data into
                 * @return success/failure
                 */
                bool Read(ByteBuf &dest) { return aws_input_stream_read(&m_underlying_stream, &dest) == 0; }

                /**
                 * Moves the head of the stream to a new location
                 * @param offset how far to move, in bytes
                 * @param seekBasis what direction to move the head of stream
                 * @return success/failure
                 */
                bool Seek(int64_t offset, StreamSeekBasis seekBasis)
                {
                    return aws_input_stream_seek(&m_underlying_stream, offset, (aws_stream_seek_basis)seekBasis) == 0;
                }

                /**
                 * Gets the stream's current status
                 * @param status output parameter for the stream's status
                 * @return success/failure
                 */
                bool GetStatus(StreamStatus &status)
                {
                    return aws_input_stream_get_status(&m_underlying_stream, &status) == 0;
                }

                /**
                 * Gets the stream's length.  Some streams may not be able to answer this.
                 * @param length output parameter for the length of the stream
                 * @return success/failure
                 */
                bool GetLength(int64_t &length)
                {
                    return aws_input_stream_get_length(&m_underlying_stream, &length) == 0;
                }

              protected:
                Allocator *m_allocator;
                aws_input_stream m_underlying_stream;

                InputStream(Aws::Crt::Allocator *allocator = ApiAllocator());

                /***
                 * Read up-to buffer::capacity - buffer::len into buffer::buffer
                 * Increment buffer::len by the amount you read in.
                 *
                 * @return true if nothing went wrong.
                 * Return true even if you read 0 bytes because the end-of-file has been reached.
                 * Return true even if you read 0 bytes because data is not currently available.
                 *
                 * Return false if an actual failure condition occurs,
                 * you SHOULD also raise an error via aws_raise_error().
                 */
                virtual bool ReadImpl(ByteBuf &buffer) noexcept = 0;

                /***
                 * Read up-to buffer::capacity - buffer::len immediately available bytes into buffer::buffer
                 * Increment buffer::len by the amount you read in.
                 *
                 * @return true if nothing went wrong.
                 * Return true even if you read 0 bytes because the end-of-file has been reached.
                 * Return true even if you read 0 bytes because data is not currently available.
                 *
                 * Return false if an actual failure condition occurs,
                 * you SHOULD also raise an error via aws_raise_error().
                 */
                virtual bool ReadSomeImpl(ByteBuf &buffer) noexcept = 0;

                /**
                 * @return the current status of the stream.
                 */
                virtual StreamStatus GetStatusImpl() const noexcept = 0;

                /**
                 * @return the total length of the available data for the stream.
                 * @return -1 if not available.
                 */
                virtual int64_t GetLengthImpl() const noexcept = 0;

                /**
                 * Seek's the stream to seekBasis based offset bytes.
                 *
                 * It is expected, that if seeking to the beginning of a stream,
                 * all error's are cleared if possible.
                 *
                 * @return true on success, false otherwise. You SHOULD raise an error via aws_raise_error()
                 * if a failure occurs.
                 */
                virtual bool SeekImpl(int64_t offset, StreamSeekBasis seekBasis) noexcept = 0;

                /**
                 * Peeks the stream
                 *
                 * Essentially calls peek on the underlying istream
                 *
                 * @return return value of the underlying istream::peek
                 */
                virtual int64_t PeekImpl() const noexcept = 0;

              private:
                static int s_Seek(aws_input_stream *stream, int64_t offset, enum aws_stream_seek_basis basis);
                static int s_Read(aws_input_stream *stream, aws_byte_buf *dest);
                static int s_GetStatus(aws_input_stream *stream, aws_stream_status *status);
                static int s_GetLength(struct aws_input_stream *stream, int64_t *out_length);
                static void s_Acquire(aws_input_stream *stream);
                static void s_Release(aws_input_stream *stream);

                static aws_input_stream_vtable s_vtable;
            };

            /***
             * Implementation of Aws::Crt::Io::InputStream that wraps a std::input_stream.
             */
            class AWS_CRT_CPP_API StdIOStreamInputStream : public InputStream
            {
              public:
                StdIOStreamInputStream(
                    std::shared_ptr<Aws::Crt::Io::IStream> stream,
                    Aws::Crt::Allocator *allocator = ApiAllocator()) noexcept;

                bool IsValid() const noexcept override;

              protected:
                bool ReadImpl(ByteBuf &buffer) noexcept override;
                bool ReadSomeImpl(ByteBuf &buffer) noexcept override;
                StreamStatus GetStatusImpl() const noexcept override;
                int64_t GetLengthImpl() const noexcept override;
                bool SeekImpl(OffsetType offsetType, StreamSeekBasis seekBasis) noexcept override;
                int64_t PeekImpl() const noexcept override;

              private:
                std::shared_ptr<Aws::Crt::Io::IStream> m_stream;
            };
        } // namespace Io
    } // namespace Crt
} // namespace Aws
