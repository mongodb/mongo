// @file bufreader.h parse a memory region into usable pieces

/**
*    Copyright (C) 2009 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#pragma once

#include <boost/noncopyable.hpp>

#include "mongo/bson/util/builder.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    /** helper to read and parse a block of memory
        methods throw the eof exception if the operation would pass the end of the
        buffer with which we are working.
    */
    class BufReader : boost::noncopyable {
    public:
        class eof : public std::exception {
        public:
            eof() { }
            virtual const char * what() const throw() { return "BufReader eof"; }
        };

        BufReader(const void *p, unsigned len) : _start(p), _pos(p), _end(((char *)_pos)+len) { }

        bool atEof() const { return _pos == _end; }

        /** read in the object specified, and advance buffer pointer */
        template <typename T>
        void read(T &t) {
            T* cur = (T*) _pos;
            T *next = cur + 1;
            if( _end < next ) throw eof();
            t = *cur;
            _pos = next;
        }

        /** read in and return an object of the specified type, and advance buffer pointer */
        template <typename T>
        T read() {
            T out;
            read(out);
            return out;
        }

        /** read in the object specified, but do not advance buffer pointer */
        template <typename T>
        void peek(T &t) const {
            T* cur = (T*) _pos;
            T *next = cur + 1;
            if( _end < next ) throw eof();
            t = *cur;
        }

        /** read in and return an object of the specified type, but do not advance buffer pointer */
        template <typename T>
        T peek() const {
            T out;
            peek(out);
            return out;
        }

        /** return current offset into buffer */
        unsigned offset() const { return (char*)_pos - (char*)_start; }

        /** return remaining bytes */
        unsigned remaining() const { return (char*)_end -(char*)_pos; }

        /** back up by nbytes */
        void rewind(unsigned nbytes) {
            _pos = ((char *) _pos) - nbytes;
            verify( _pos >= _start );
        }

        /** return current position pointer, and advance by len */
        const void* skip(unsigned len) {
            const char *nxt = ((char *) _pos) + len;
            if( _end < nxt ) throw eof();
            const void *p = _pos;
            _pos = nxt;
            return p;
        }

        /// reads a NUL terminated string
        StringData readCStr() {
            const char* start = static_cast<const char*>(pos());
            size_t len = strnlen(start, remaining()-1);
            if (start[len] != '\0') throw eof(); // no NUL byte in remaining bytes
            skip(len + 1/*NUL byte*/);
            return StringData(start, len);
        }

        void readStr(string& s) {
            s = readCStr().toString();
        }

        const void* pos() { return _pos; }
        const void* start() { return _start; }

    private:
        const void *_start;
        const void *_pos;
        const void *_end;
    };

}
