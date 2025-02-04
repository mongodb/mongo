/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/RequestCompression.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <algorithm>
#include <aws/core/utils/memory/stl/AWSAllocator.h>

#ifdef ENABLED_ZLIB_REQUEST_COMPRESSION
#include "zlib.h"

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#include <fcntl.h>
#include <io.h>
#define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#define SET_BINARY_MODE(file)
#endif // defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
// Defining zlib chunks to be 256k
static const size_t ZLIB_CHUNK=263144;
static const char AWS_REQUEST_COMPRESSION_ALLOCATION_TAG[] =
    "RequestCompressionAlloc";
#endif // ENABLED_ZLIB_REQUEST_COMPRESSION

static const char AWS_REQUEST_COMPRESSION_LOG_TAG[] = "RequestCompression";

Aws::String Aws::Client::GetCompressionAlgorithmId(const CompressionAlgorithm &algorithm)
{
    switch (algorithm)
    {
    case CompressionAlgorithm::GZIP:
        return "gzip";
    default:
        return "";
    }
}

#ifdef ENABLED_ZLIB_REQUEST_COMPRESSION
#ifdef USE_AWS_MEMORY_MANAGEMENT
static const char* ZlibMemTag = "zlib";
static const size_t offset = sizeof(size_t); // to make space for size of the array
//Define custom memory allocation for zlib
// if fail to allocate, return Z_NULL
void* aws_zalloc(void * /* opaque */, unsigned items, unsigned size)
{
    unsigned sizeToAllocate = items*size;
    size_t sizeToAllocateWithOffset = sizeToAllocate + offset;
    if ((size != 0 && sizeToAllocate / size != items)
        || (sizeToAllocateWithOffset <= sizeToAllocate ))
    {
        return Z_NULL;
    }
    char* newMem = reinterpret_cast<char*>(Aws::Malloc(ZlibMemTag, sizeToAllocateWithOffset));
    if (newMem != nullptr) {
        std::size_t* pointerToSize = reinterpret_cast<std::size_t*>(newMem);
        *pointerToSize = size;
        return reinterpret_cast<void*>(newMem + offset);
    }
    else
    {
        return Z_NULL;
    }
}

void aws_zfree(void * /* opaque */, void * ptr)
{
    if(ptr)
    {
        char* shiftedMemory = reinterpret_cast<char*>(ptr);
        Aws::Free(shiftedMemory - offset);
    }
}

#endif // USE_AWS_MEMORY_MANAGEMENT
#endif // ENABLED_ZLIB_REQUEST_COMPRESSION


iostream_outcome Aws::Client::RequestCompression::compress(std::shared_ptr<Aws::IOStream> input,
                                                           const CompressionAlgorithm &algorithm) const
{
#ifdef ENABLED_ZLIB_REQUEST_COMPRESSION
    if (algorithm == CompressionAlgorithm::GZIP)
    {
        // calculating stream size
        input->seekg(0, input->end);
        size_t streamSize = input->tellg();
        input->seekg(0, input->beg);

        AWS_LOGSTREAM_TRACE(AWS_REQUEST_COMPRESSION_LOG_TAG, "Compressing request of " << streamSize << " bytes.");

        // Preparing output
        std::shared_ptr<Aws::IOStream> output = Aws::MakeShared<Aws::StringStream>(AWS_REQUEST_COMPRESSION_ALLOCATION_TAG);
        if(!output)
        {
           AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Failed to allocate output while compressing")
           return false;
        }
        // Prepare ZLIB to compress
        int ret = Z_NULL;
        int flush = Z_NO_FLUSH;
        z_stream strm = {};
        auto in = Aws::MakeUniqueArray<unsigned char>(ZLIB_CHUNK, AWS_REQUEST_COMPRESSION_ALLOCATION_TAG);
        if(!in)
        {
           AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Failed to allocate in buffer while compressing")
           return false;
        }

        auto out = Aws::MakeUniqueArray<unsigned char>(ZLIB_CHUNK, AWS_REQUEST_COMPRESSION_ALLOCATION_TAG);
        if(!out)
        {
           AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Failed to allocate out buffer while compressing")
           return false;
        }

        //Preparing allocators
#ifdef USE_AWS_MEMORY_MANAGEMENT
        strm.zalloc = (void *(*)(void *, unsigned, unsigned)) aws_zalloc;
        strm.zfree = (void (*)(void *, void *)) aws_zfree;
#else
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
#endif
        strm.opaque = Z_NULL;

        const int MAX_WINDOW_GZIP = 31;
        const int DEFAULT_MEM_LEVEL_USAGE = 8;
        ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WINDOW_GZIP, DEFAULT_MEM_LEVEL_USAGE, Z_DEFAULT_STRATEGY);
        if(ret != Z_OK)
        {
           return false;
        }

        //Adding one to the stream size counter to account for the EOF marker.
        streamSize++;
        size_t toRead = 0;
        // Compress
        do {
            toRead = std::min(streamSize, ZLIB_CHUNK);
            // Fill the buffer
            if (! input->read(reinterpret_cast<char *>(in.get()), toRead))
            {
                if (input->eof())
                {
                    //Last read need flush when exit loop
                    flush = Z_FINISH;
                }
                else {
                    AWS_LOGSTREAM_ERROR(
                        AWS_REQUEST_COMPRESSION_LOG_TAG,
                        "Uncompress request failed to read from stream");
                    return false;
                }
            }
            assert(streamSize >= toRead);
            streamSize -= toRead; //left to read
            strm.avail_in = static_cast<uInt>((flush == Z_FINISH) ? toRead-1 : toRead); //skip EOF if included
            strm.next_in = in.get();
            do
            {
                // Run deflate on buffers
                strm.avail_out = ZLIB_CHUNK;
                strm.next_out = out.get();

                ret = deflate(&strm, flush);

                // writing the output
                assert(ZLIB_CHUNK >= strm.avail_out);
                unsigned output_size = ZLIB_CHUNK - strm.avail_out;
                if(! output->write(reinterpret_cast<char *>(out.get()), output_size))
                {
                    AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Compressed request failed to write to output stream");
                    return false;
                }
            } while (strm.avail_out == 0);
            assert(strm.avail_in == 0); // All data was read
        } while (flush != Z_FINISH);
        assert(ret == Z_STREAM_END); // Completed stream
        AWS_LOGSTREAM_TRACE(AWS_REQUEST_COMPRESSION_LOG_TAG, "Compressed request to: " << strm.total_out << " bytes");
        deflateEnd(&strm);
        return output;
    }
    else
    {
        AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Compress request requested in runtime without support: " << GetCompressionAlgorithmId(algorithm));
        return false;
    }
#else
    // If there is no support to compress, always fail calls to this method.
    AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Compress request requested in runtime without support: " << GetCompressionAlgorithmId(algorithm));
    AWS_UNREFERENCED_PARAM(input);     // silencing warning;
    return false;
#endif
}

Aws::Utils::Outcome<std::shared_ptr<Aws::IOStream>, bool>
Aws::Client::RequestCompression::uncompress(std::shared_ptr<Aws::IOStream> input, const CompressionAlgorithm &algorithm) const
{
#ifdef ENABLED_ZLIB_REQUEST_COMPRESSION
    if (algorithm == CompressionAlgorithm::GZIP)
    {
        // calculating stream size
        input->seekg(0, input->end);
        size_t streamSize = input->tellg();
        input->seekg(0, input->beg);

        AWS_LOGSTREAM_TRACE(AWS_REQUEST_COMPRESSION_LOG_TAG, "Uncompressing request of " << streamSize << " bytes.");

        // Preparing output
        std::shared_ptr<Aws::IOStream> output = Aws::MakeShared<Aws::StringStream>( AWS_REQUEST_COMPRESSION_ALLOCATION_TAG);
        if(!output)
        {
            AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Failed to allocate output while uncompressing")
            return false;
        }

        // Prepare ZLIB to uncompress
        int ret = Z_NULL;
        z_stream strm = {};
        auto in = Aws::MakeUniqueArray<unsigned char>(ZLIB_CHUNK, AWS_REQUEST_COMPRESSION_ALLOCATION_TAG);
        if(!in)
        {
            AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Failed to allocate in buffer while uncompressing")
            return false;
        }

        auto out = Aws::MakeUniqueArray<unsigned char>(ZLIB_CHUNK, AWS_REQUEST_COMPRESSION_ALLOCATION_TAG);
        if(!out)
        {
            AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Failed to allocate out buffer while uncompressing")
            return false;
        }

        //preparing allocation
#ifdef USE_AWS_MEMORY_MANAGEMENT
        strm.zalloc = (void *(*)(void *, unsigned, unsigned)) aws_zalloc;
        strm.zfree = (void (*)(void *, void *)) aws_zfree;
#else
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
#endif
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;

        const int MAX_WINDOW_GZIP = 31;
        ret = inflateInit2(&strm, MAX_WINDOW_GZIP);
        if (ret != Z_OK)
        {
            return false;
        }

        //Adding one to the stream size counter to account for the EOF marker.
        streamSize++;
        size_t toRead = 0;
        // Decompress
        do {
            toRead = (streamSize < ZLIB_CHUNK)?streamSize:ZLIB_CHUNK;
            if (toRead < 1) break; // Nothing left to read
            // Fill the buffer
            if(! input->read(reinterpret_cast<char *>(in.get()), toRead))
            {
                if (input->eof())
                {
                    //skip passing the EOF to the buffer
                    toRead--;
                }
                else
                {
                    AWS_LOGSTREAM_ERROR(
                        AWS_REQUEST_COMPRESSION_LOG_TAG,
                        "Compress request failed to read from stream");
                    return false;
                }
            }

            // Filling input buffer to decompress
            strm.avail_in = static_cast<uInt>(toRead);
            strm.next_in = in.get();
            do
            {
                // Run inflate on buffers
                strm.avail_out = ZLIB_CHUNK;
                strm.next_out = out.get();

                ret = inflate(&strm, Z_NO_FLUSH);
                // Catch errors
                switch (ret)
                {
                case Z_NEED_DICT:
                    AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Compressed request failed to inflate with code: Z_NEED_DICT");
                    return false;
                case Z_DATA_ERROR:
                    AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Compressed request failed to inflate with code: Z_DATA_ERROR");
                    return false;
                case Z_MEM_ERROR:
                    (void)inflateEnd(&strm);
                    AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Compressed request failed to inflate with code: Z_MEM_ERROR");
                    return false;
                }

                // writing the output
                unsigned output_size = ZLIB_CHUNK - strm.avail_out;
                if(! output->write(reinterpret_cast<char *>(out.get()), output_size)) {
                    AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Uncompressed request failed to write to output stream");
                    return false;
                }
            } while (strm.avail_out == 0);
        } while (ret != Z_STREAM_END);
        // clean up
        (void)inflateEnd(&strm);
        if (ret == Z_STREAM_END)
        {
            AWS_LOGSTREAM_TRACE(AWS_REQUEST_COMPRESSION_LOG_TAG, "Decompressed request to: " << strm.total_out << " bytes");
            return output;
        }
        else
        {
            AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Failed to decompress after read input completely");
            return false;
        }
    }
    else
    {
        AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Uncompress request requested in runtime without support: " << GetCompressionAlgorithmId(algorithm));
        return false;
    }
#else
    // If there is no support to compress, always fail calls to this method.
    AWS_LOGSTREAM_ERROR(AWS_REQUEST_COMPRESSION_LOG_TAG, "Uncompress request requested in runtime without support: " << GetCompressionAlgorithmId(algorithm));
    AWS_UNREFERENCED_PARAM(input);     // silencing warning;
    return false;
#endif
}
