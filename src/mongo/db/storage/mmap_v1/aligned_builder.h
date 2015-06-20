/**
 *    Copyright (C) 2009-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"

namespace mongo {

/** a page-aligned BufBuilder. */
class AlignedBuilder {
public:
    AlignedBuilder(unsigned init_size);
    ~AlignedBuilder() {
        kill();
    }

    /** reset with a hint as to the upcoming needed size specified */
    void reset(unsigned sz);

    /** reset for a re-use. shrinks if > 128MB */
    void reset();

    /** note this may be deallocated (realloced) if you keep writing or reset(). */
    const char* buf() const {
        return _p._data;
    }

    /** leave room for some stuff later
        @return offset in the buffer that was our current position
    */
    size_t skip(unsigned n) {
        unsigned l = len();
        grow(n);
        return l;
    }

    /** if buffer grows pointer no longer valid */
    char* atOfs(unsigned ofs) {
        return _p._data + ofs;
    }

    /** if buffer grows pointer no longer valid */
    char* cur() {
        return _p._data + _len;
    }

    void appendChar(char j) {
        *((char*)grow(sizeof(char))) = j;
    }
    void appendNum(char j) {
        *((char*)grow(sizeof(char))) = j;
    }
    void appendNum(short j) {
        *((short*)grow(sizeof(short))) = j;
    }
    void appendNum(int j) {
        *((int*)grow(sizeof(int))) = j;
    }
    void appendNum(unsigned j) {
        *((unsigned*)grow(sizeof(unsigned))) = j;
    }
    void appendNum(bool j) {
        *((bool*)grow(sizeof(bool))) = j;
    }
    void appendNum(double j) {
        *((double*)grow(sizeof(double))) = j;
    }
    void appendNum(long long j) {
        *((long long*)grow(sizeof(long long))) = j;
    }
    void appendNum(unsigned long long j) {
        *((unsigned long long*)grow(sizeof(unsigned long long))) = j;
    }

    void appendBuf(const void* src, size_t len) {
        memcpy(grow((unsigned)len), src, len);
    }

    template <class T>
    void appendStruct(const T& s) {
        appendBuf(&s, sizeof(T));
    }

    void appendStr(StringData str, bool includeEOO = true) {
        const unsigned len = str.size() + (includeEOO ? 1 : 0);
        verify(len < (unsigned)BSONObjMaxUserSize);
        str.copyTo(grow(len), includeEOO);
    }

    /** @return the in-use length */
    unsigned len() const {
        return _len;
    }

private:
    static const unsigned Alignment = 8192;

    /** returns the pre-grow write position */
    inline char* grow(unsigned by) {
        unsigned oldlen = _len;
        _len += by;
        if (MONGO_unlikely(_len > _p._size)) {
            growReallocate(oldlen);
        }
        return _p._data + oldlen;
    }

    void growReallocate(unsigned oldLenInUse);
    void kill();
    void mallocSelfAligned(unsigned sz);
    void _malloc(unsigned sz);
    void _realloc(unsigned newSize, unsigned oldLenInUse);
    void _free(void*);

    struct AllocationInfo {
        char* _data;
        void* _allocationAddress;
        unsigned _size;
    } _p;
    unsigned _len;  // bytes in use
};
}
