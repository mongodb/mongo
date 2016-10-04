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

#include "mongo/base/data_type.h"

namespace mongo {

template <char C, typename T>
struct Terminated {
    Terminated() : value(DataType::defaultConstruct<T>()) {}
    Terminated(T value) : value(std::move(value)) {}
    T value;

    operator T() const {
        return value;
    }
};

struct TerminatedHelper {
    static Status makeLoadNoTerminalStatus(char c, size_t length, std::ptrdiff_t debug_offset);
    static Status makeLoadShortReadStatus(char c,
                                          size_t read,
                                          size_t length,
                                          std::ptrdiff_t debug_offset);
    static Status makeStoreStatus(char c, size_t length, std::ptrdiff_t debug_offset);
};

template <char C, typename T>
struct DataType::Handler<Terminated<C, T>> {
    using TerminatedType = Terminated<C, T>;

    static Status load(TerminatedType* tt,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        size_t local_advanced = 0;

        const char* end = static_cast<const char*>(std::memchr(ptr, C, length));

        if (!end) {
            return TerminatedHelper::makeLoadNoTerminalStatus(C, length, debug_offset);
        }

        auto status = DataType::load(
            tt ? &tt->value : nullptr, ptr, end - ptr, &local_advanced, debug_offset);

        if (!status.isOK()) {
            return status;
        }

        if (local_advanced != static_cast<size_t>(end - ptr)) {
            return TerminatedHelper::makeLoadShortReadStatus(
                C, local_advanced, end - ptr, debug_offset);
        }

        if (advanced) {
            *advanced = local_advanced + 1;
        }

        return Status::OK();
    }

    static Status store(const TerminatedType& tt,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) {
        size_t local_advanced = 0;

        auto status = DataType::store(tt.value, ptr, length, &local_advanced, debug_offset);

        if (!status.isOK()) {
            return status;
        }

        if (length - local_advanced < 1) {
            return TerminatedHelper::makeStoreStatus(C, length, debug_offset + local_advanced);
        }

        ptr[local_advanced] = C;

        if (advanced) {
            *advanced = local_advanced + 1;
        }

        return Status::OK();
    }

    static TerminatedType defaultConstruct() {
        return TerminatedType();
    }
};

}  // namespace mongo
