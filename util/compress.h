// @file compress.h

#pragma once

#include "alignedbuilder.h"

namespace mongo {

    /** instead of just taking a big byte buf and compressing it, here we give a builder-style interface for 
        two reasons: 
        (1) can save double-copying this way, potentially
        (2) can potentially leverage the semantic information present in the various calls.  for example appendNum() 
            is quite different from appendStruct or appendStr the compressor might leverage that for efficiency.
    */
    class CompressedBuilder {
        AlignedBuilder b;
    public:
        CompressedBuilder(unsigned init_size) : b(init_size) { }

        /** reset for a re-use */
        void reset() { b.reset(); }

        void appendChar(char j) { b.appendChar(j); }
        void appendNum(char j) { b.appendNum(j); }
        void appendNum(short j) { b.appendNum(j); }
        void appendNum(int j) { b.appendNum(j); }
        void appendNum(unsigned j) { b.appendNum(j); }
        void appendNum(bool j) { b.appendNum(j); }
        void appendNum(double j) { b.appendNum(j); }
        void appendNum(long long j) { b.appendNum(j); }
        void appendNum(unsigned long long j) { b.appendNum(j); }

        void appendBuf(const void *src, size_t len) { b.appendBuf(src,len); }

        template<class T>
        void appendStruct(const T& s) { b.appendStruct(s); }

        void appendStr(const StringData &str , bool includeEOO = true ) {
            b.appendStr(str, includeEOO);
        }

        /** @return the in-use length */
        unsigned len() const { return b.len(); }

        const char* buf() const { return b.buf(); }

        void pad(unsigned padding) { b.skip(padding); }

        void reserve32() { 
            dassert( b.len() == 0 );
            b.skip(4);
        }
        void backfill32(unsigned x) {
            *((unsigned*)b.atOfs(0)) = x;
        }

    };

}
