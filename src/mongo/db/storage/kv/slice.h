// kv_dictionary.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include <algorithm>
#include <string>

#include <boost/shared_array.hpp>

#include "mongo/util/assert_util.h"

namespace mongo {

    /**
     * Slice is similar to a std::string that may or may not own its own
     * memory.  It is similar to BSONObj in that you can get a copy that
     * does own its own memory, and if the original already owned it, the
     * copy will share ownership of the owned buffer.
     *
     * Some sensible default constructors are provided, as well as copy
     * and move constructors/assignment operators.
     *
     * To convert between a POD object and a Slice referencing such an
     * object, use Slice::of(val) and slice.as<T>().
     *
     * To write a fresh slice, construct a Slice with just a size argument
     * to reserve some space, then write into mutableData().
     *
     * You can use algorithms that use RandomAccessIterators, if you want
     * this for some reason.
     */
    class Slice {
    public:
        Slice()
            : _data(NULL),
              _size(0)
        {}

        explicit Slice(size_t sz)
            : _buf(new char[sz]),
              _data(_buf.get()),
              _size(sz)
        {}

        Slice(const char *p, size_t sz)
            : _data(p),
              _size(sz)
        {}

        explicit Slice(const std::string &str)
            : _data(str.c_str()),
              _size(str.size())
        {}

        Slice(const Slice &other)
            : _buf(other._buf),
              _data(other._data),
              _size(other._size)
        {}

        Slice& operator=(const Slice &other) {
            _buf = other._buf;
            _data = other._data;
            _size = other._size;
            return *this;
        }

#if __cplusplus >= 201103L
        Slice(Slice&& other)
            : _buf(),
              _data(NULL),
              _size(0)
        {
            std::swap(_buf, other._buf);
            std::swap(_data, other._data);
            std::swap(_size, other._size);
        }

        Slice& operator=(Slice&& other) {
            std::swap(_buf, other._buf);
            std::swap(_data, other._data);
            std::swap(_size, other._size);
            return *this;
        }
#endif

        template<typename T>
        static Slice of(const T &v) {
            return Slice(reinterpret_cast<const char *>(&v), sizeof v);
        }

        template<typename T>
        T as() const {
            invariant(size() == sizeof(T));
            const T *p = reinterpret_cast<const T *>(data());
            return *p;
        }

        const char *data() const { return _data; }

        char *mutableData() const {
            return _buf.get();
        }

        boost::shared_array<char>& ownedBuf() {
            return _buf;
        }

        size_t size() const { return _size; }

        bool empty() const { return size() == 0; }

        Slice copy() const {
            Slice s(size());
            std::copy(begin(), end(), s.begin());
            return s;
        }

        Slice owned() const {
            if (_buf) {
                return *this;
            } else {
                return copy();
            }
        }

        bool operator==(const Slice &o) const {
            return data() == o.data() && size() == o.size();
        }

        char *begin() { return mutableData(); }
        char *end() { return mutableData() + size(); }
        char *rbegin() { return end(); }
        char *rend() { return begin(); }
        const char *begin() const { return data(); }
        const char *end() const { return data() + size(); }
        const char *rbegin() const { return end(); }
        const char *rend() const { return begin(); }
        const char *cbegin() const { return data(); }
        const char *cend() const { return data() + size(); }
        const char *crbegin() const { return end(); }
        const char *crend() const { return begin(); }

    private:
        boost::shared_array<char> _buf;
        const char *_data;
        size_t _size;
    };

} // namespace mongo

namespace std {

    template<>
    class iterator_traits<mongo::Slice> {
        typedef typename std::iterator_traits<const char *>::difference_type difference_type;
        typedef typename std::iterator_traits<const char *>::value_type value_type;
        typedef typename std::iterator_traits<const char *>::pointer pointer;
        typedef typename std::iterator_traits<const char *>::reference reference;
        typedef typename std::iterator_traits<const char *>::iterator_category iterator_category;
    };

} // namespace std
