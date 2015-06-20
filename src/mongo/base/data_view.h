/*    Copyright 2014 MongoDB Inc.
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

#include <cstring>
#include <type_traits>

#include "mongo/config.h"

#include "mongo/base/data_type.h"

namespace mongo {

class ConstDataView {
public:
    typedef const char* bytes_type;

    ConstDataView(bytes_type bytes) : _bytes(bytes) {}

    bytes_type view(std::size_t offset = 0) const {
        return _bytes + offset;
    }

    template <typename T>
    const ConstDataView& read(T* t, size_t offset = 0) const {
        DataType::unsafeLoad(t, view(offset), nullptr);

        return *this;
    }

    template <typename T>
    T read(std::size_t offset = 0) const {
        T t(DataType::defaultConstruct<T>());

        read(&t, offset);

        return t;
    }

private:
    bytes_type _bytes;
};

class DataView : public ConstDataView {
public:
    typedef char* bytes_type;

    DataView(bytes_type bytes) : ConstDataView(bytes) {}

    bytes_type view(std::size_t offset = 0) const {
        // It is safe to cast away const here since the pointer stored in our base class was
        // originally non-const by way of our constructor.
        return const_cast<bytes_type>(ConstDataView::view(offset));
    }

    template <typename T>
    DataView& write(const T& value, std::size_t offset = 0) {
        DataType::unsafeStore(value, view(offset), nullptr);

        return *this;
    }
};

}  // namespace mongo
