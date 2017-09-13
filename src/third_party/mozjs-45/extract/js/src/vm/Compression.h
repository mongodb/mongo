/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Compression_h
#define vm_Compression_h

#include <zlib.h>

#include "jstypes.h"

namespace js {

class Compressor
{
    /* Number of bytes we should hand to zlib each compressMore() call. */
    static const size_t CHUNKSIZE = 2048;
    z_stream zs;
    const unsigned char* inp;
    size_t inplen;
    size_t outbytes;
    bool initialized;

  public:
    enum Status {
        MOREOUTPUT,
        DONE,
        CONTINUE,
        OOM
    };

    Compressor(const unsigned char* inp, size_t inplen);
    ~Compressor();
    bool init();
    void setOutput(unsigned char* out, size_t outlen);
    size_t outWritten() const { return outbytes; }
    /* Compress some of the input. Return true if it should be called again. */
    Status compressMore();
};

/*
 * Decompress a string. The caller must know the length of the output and
 * allocate |out| to a string of that length.
 */
bool DecompressString(const unsigned char* inp, size_t inplen,
                      unsigned char* out, size_t outlen);

} /* namespace js */

#endif /* vm_Compression_h */
