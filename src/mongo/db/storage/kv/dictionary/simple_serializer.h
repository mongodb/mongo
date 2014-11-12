// simple_serializer.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#if __cplusplus >= 201103L
#include <type_traits>
#endif

// TODO: This probably belongs in mongo/util.

namespace mongo {

    /**
     * Convenience class for writing object data to a char buffer.
     */
    class BufferWriter {
    public:
        explicit BufferWriter(char *dest)
            : _dest(dest)
        {}

        char *get() const { return _dest; }

        /**
         * Write a `T' into the buffer and advance the internal buffer
         * position by sizeof(T) bytes.
         *
         * Requires: the `dest' buf has enough space for writing.
         */
        template<typename T>
        BufferWriter& write(const T &val) {
#if MONGO_HAVE_STD_IS_TRIVIALLY_COPYABLE
            static_assert(std::is_trivially_copyable<T>::value,
                          "Type for BufferWriter::write must be trivially copyable");
#endif
            T *tdest = reinterpret_cast<T *>(_dest);
            _dest += sizeof *tdest;
            *tdest = val;
            return *this;
        }

    private:
        char *_dest;
    };

    /**
     * Convenience class for reading object data from a char buffer.
     */
    class BufferReader {
    public:
        explicit BufferReader(const char *src)
            : _src(src)
        {}

        const char *get() const { return _src; }

        /**
         * Read and return a `T' from the buffer and advance the internal
         * buffer position by sizeof(T);
         *
         * Requires: the `src' buf has more data for reading.
         */
        template<typename T>
        T read() {
#if MONGO_HAVE_STD_IS_TRIVIALLY_COPYABLE
            static_assert(std::is_trivially_copyable<T>::value,
                          "Type for BufferReader::read must be trivially copyable");
#endif
            const T *tsrc = reinterpret_cast<const T *>(_src);
            _src += sizeof *tsrc;
            return *tsrc;
        }

    private:
        const char *_src;
    };

} // namespace mongo
