/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Compression.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/PodOperations.h"
#include "mozilla/ScopeExit.h"

#include "jsutil.h"

#include "js/Utility.h"

using namespace js;

static void*
zlib_alloc(void* cx, uInt items, uInt size)
{
    return js_calloc(items, size);
}

static void
zlib_free(void* cx, void* addr)
{
    js_free(addr);
}

Compressor::Compressor(const unsigned char* inp, size_t inplen)
    : inp(inp),
      inplen(inplen),
      initialized(false),
      finished(false),
      currentChunkSize(0),
      chunkOffsets()
{
    MOZ_ASSERT(inplen > 0);
    zs.opaque = nullptr;
    zs.next_in = (Bytef*)inp;
    zs.avail_in = 0;
    zs.next_out = nullptr;
    zs.avail_out = 0;
    zs.zalloc = zlib_alloc;
    zs.zfree = zlib_free;

    // Reserve space for the CompressedDataHeader.
    outbytes = sizeof(CompressedDataHeader);
}

Compressor::~Compressor()
{
    if (initialized) {
        int ret = deflateEnd(&zs);
        if (ret != Z_OK) {
            // If we finished early, we can get a Z_DATA_ERROR.
            MOZ_ASSERT(ret == Z_DATA_ERROR);
            MOZ_ASSERT(!finished);
        }
    }
}

// According to the zlib docs, the default value for windowBits is 15. Passing
// -15 is treated the same, but it also forces 'raw deflate' (no zlib header or
// trailer). Raw deflate is necessary for chunked decompression.
static const int WindowBits = -15;

bool
Compressor::init()
{
    if (inplen >= UINT32_MAX)
        return false;
    // zlib is slow and we'd rather be done compression sooner
    // even if it means decompression is slower which penalizes
    // Function.toString()
    int ret = deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, WindowBits, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        MOZ_ASSERT(ret == Z_MEM_ERROR);
        return false;
    }
    initialized = true;
    return true;
}

void
Compressor::setOutput(unsigned char* out, size_t outlen)
{
    MOZ_ASSERT(outlen > outbytes);
    zs.next_out = out + outbytes;
    zs.avail_out = outlen - outbytes;
}

Compressor::Status
Compressor::compressMore()
{
    MOZ_ASSERT(zs.next_out);
    uInt left = inplen - (zs.next_in - inp);
    if (left <= MAX_INPUT_SIZE)
        zs.avail_in = left;
    else if (zs.avail_in == 0)
        zs.avail_in = MAX_INPUT_SIZE;

    // Finish the current chunk if needed.
    bool flush = false;
    MOZ_ASSERT(currentChunkSize <= CHUNK_SIZE);
    if (currentChunkSize + zs.avail_in >= CHUNK_SIZE) {
        // Adjust avail_in, so we don't get chunks that are larger than
        // CHUNK_SIZE.
        zs.avail_in = CHUNK_SIZE - currentChunkSize;
        MOZ_ASSERT(currentChunkSize + zs.avail_in == CHUNK_SIZE);
        flush = true;
    }

    MOZ_ASSERT(zs.avail_in <= left);
    bool done = zs.avail_in == left;

    Bytef* oldin = zs.next_in;
    Bytef* oldout = zs.next_out;
    int ret = deflate(&zs, done ? Z_FINISH : (flush ? Z_FULL_FLUSH : Z_NO_FLUSH));
    outbytes += zs.next_out - oldout;
    currentChunkSize += zs.next_in - oldin;
    MOZ_ASSERT(currentChunkSize <= CHUNK_SIZE);

    if (ret == Z_MEM_ERROR) {
        zs.avail_out = 0;
        return OOM;
    }
    if (ret == Z_BUF_ERROR || (ret == Z_OK && zs.avail_out == 0)) {
        // We have to resize the output buffer. Note that we're not done yet
        // because ret != Z_STREAM_END.
        MOZ_ASSERT(zs.avail_out == 0);
        return MOREOUTPUT;
    }

    if (done || currentChunkSize == CHUNK_SIZE) {
        MOZ_ASSERT_IF(!done, flush);
        MOZ_ASSERT(chunkSize(inplen, chunkOffsets.length()) == currentChunkSize);
        if (!chunkOffsets.append(outbytes))
            return OOM;
        currentChunkSize = 0;
        MOZ_ASSERT_IF(done, chunkOffsets.length() == (inplen - 1) / CHUNK_SIZE + 1);
    }

    MOZ_ASSERT_IF(!done, ret == Z_OK);
    MOZ_ASSERT_IF(done, ret == Z_STREAM_END);
    return done ? DONE : CONTINUE;
}

size_t
Compressor::totalBytesNeeded() const
{
    return AlignBytes(outbytes, sizeof(uint32_t)) + sizeOfChunkOffsets();
}

void
Compressor::finish(char* dest, size_t destBytes)
{
    MOZ_ASSERT(!chunkOffsets.empty());

    CompressedDataHeader* compressedHeader = reinterpret_cast<CompressedDataHeader*>(dest);
    compressedHeader->compressedBytes = outbytes;

    size_t outbytesAligned = AlignBytes(outbytes, sizeof(uint32_t));

    // Zero the padding bytes, the ImmutableStringsCache will hash them.
    mozilla::PodZero(dest + outbytes, outbytesAligned - outbytes);

    uint32_t* destArr = reinterpret_cast<uint32_t*>(dest + outbytesAligned);

    MOZ_ASSERT(uintptr_t(dest + destBytes) == uintptr_t(destArr + chunkOffsets.length()));
    mozilla::PodCopy(destArr, chunkOffsets.begin(), chunkOffsets.length());

    finished = true;
}

bool
js::DecompressString(const unsigned char* inp, size_t inplen, unsigned char* out, size_t outlen)
{
    MOZ_ASSERT(inplen <= UINT32_MAX);

    // Mark the memory we pass to zlib as initialized for MSan.
    MOZ_MAKE_MEM_DEFINED(out, outlen);

    z_stream zs;
    zs.zalloc = zlib_alloc;
    zs.zfree = zlib_free;
    zs.opaque = nullptr;
    zs.next_in = (Bytef*)inp;
    zs.avail_in = inplen;
    zs.next_out = out;
    MOZ_ASSERT(outlen);
    zs.avail_out = outlen;
    int ret = inflateInit(&zs);
    if (ret != Z_OK) {
        MOZ_ASSERT(ret == Z_MEM_ERROR);
        return false;
    }
    ret = inflate(&zs, Z_FINISH);
    MOZ_ASSERT(ret == Z_STREAM_END);
    ret = inflateEnd(&zs);
    MOZ_ASSERT(ret == Z_OK);
    return true;
}

bool
js::DecompressStringChunk(const unsigned char* inp, size_t chunk,
                          unsigned char* out, size_t outlen)
{
    MOZ_ASSERT(outlen <= Compressor::CHUNK_SIZE);

    const CompressedDataHeader* header = reinterpret_cast<const CompressedDataHeader*>(inp);

    size_t compressedBytes = header->compressedBytes;
    size_t compressedBytesAligned = AlignBytes(compressedBytes, sizeof(uint32_t));

    const unsigned char* offsetBytes = inp + compressedBytesAligned;
    const uint32_t* offsets = reinterpret_cast<const uint32_t*>(offsetBytes);

    uint32_t compressedStart = chunk > 0 ? offsets[chunk - 1] : sizeof(CompressedDataHeader);
    uint32_t compressedEnd = offsets[chunk];

    MOZ_ASSERT(compressedStart < compressedEnd);
    MOZ_ASSERT(compressedEnd <= compressedBytes);

    bool lastChunk = compressedEnd == compressedBytes;

    // Mark the memory we pass to zlib as initialized for MSan.
    MOZ_MAKE_MEM_DEFINED(out, outlen);

    z_stream zs;
    zs.zalloc = zlib_alloc;
    zs.zfree = zlib_free;
    zs.opaque = nullptr;
    zs.next_in = (Bytef*)(inp + compressedStart);
    zs.avail_in = compressedEnd - compressedStart;
    zs.next_out = out;
    MOZ_ASSERT(outlen);
    zs.avail_out = outlen;

    int ret = inflateInit2(&zs, WindowBits);
    if (ret != Z_OK) {
        MOZ_ASSERT(ret == Z_MEM_ERROR);
        return false;
    }

    auto autoCleanup = mozilla::MakeScopeExit([&] {
        mozilla::DebugOnly<int> ret = inflateEnd(&zs);
        MOZ_ASSERT(ret == Z_OK);
    });

    if (lastChunk) {
        ret = inflate(&zs, Z_FINISH);
        MOZ_RELEASE_ASSERT(ret == Z_STREAM_END);
    } else {
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_MEM_ERROR)
            return false;
        MOZ_RELEASE_ASSERT(ret == Z_OK);
    }
    MOZ_ASSERT(zs.avail_in == 0);
    MOZ_ASSERT(zs.avail_out == 0);
    return true;
}
