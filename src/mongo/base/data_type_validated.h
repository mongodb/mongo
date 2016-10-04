/*    Copyright 2015 MongoDB Inc.
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

#include <utility>

#include "mongo/base/data_type.h"

namespace mongo {

/**
 * Allows for specializations of load/store that run validation logic.
 *
 * To add validation for your T:
 * 1) ensure that there are DataType::Handler<T> specializations for your type
 * 2) implement a specialization of Validator<T> for your type. The two methods
 * you must implement are:
 *    - Status validateLoad(const char* ptr, size_t length);
 *    - Status validateStore(const T& toStore);
 *
 * See bson_validate.h for an example.
 *
 * Then you can use Validated<T> in a DataRange (and associated types)
 *
 * Example:
 *
 *     DataRangeCursor drc(buf, buf_end);
 *     Validated<MyObj> vobj;
 *     auto status = drc.readAndAdvance(&vobj);
 *     if (status.isOK()) {
 *         // use vobj.val
 *         // ....
 *     }
 */
template <typename T>
struct Validator {
    // These methods are intentionally unimplemented so that if the default validator
    // is instantiated, the resulting binary will not link.

    /**
     * Checks that the provided buffer contains at least 1 valid object of type T.
     * The length parameter is the size of the buffer, not the size of the object.
     * Specializations of this function should be hardened to malicious input from untrusted
     * sources.
     */
    static Status validateLoad(const char* ptr, size_t length);

    /**
     * Checks that the provided object is valid to store in a buffer.
     */
    static Status validateStore(const T& toStore);
};

template <typename T>
struct Validated {
    Validated() = default;
    Validated(T value) : val(std::move(value)) {}

    operator T&() {
        return val;
    }

    T val = DataType::defaultConstruct<T>();
};

template <typename T>
struct DataType::Handler<Validated<T>> {
    static Status load(Validated<T>* vt,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        size_t local_advanced = 0;

        auto valid = Validator<T>::validateLoad(ptr, length);

        if (!valid.isOK()) {
            return valid;
        }

        auto loadStatus =
            DataType::load(vt ? &vt->val : nullptr, ptr, length, &local_advanced, debug_offset);

        if (!loadStatus.isOK()) {
            return loadStatus;
        }

        if (advanced) {
            *advanced = local_advanced;
        }

        return Status::OK();
    }

    static Status store(const Validated<T>& vt,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) {
        size_t local_advanced = 0;

        auto valid = Validator<T>::validateStore(vt.val);

        if (!valid.isOK()) {
            return valid;
        }

        auto storeStatus = DataType::store(vt.val, ptr, length, &local_advanced, debug_offset);

        if (!storeStatus.isOK()) {
            return storeStatus;
        }

        if (advanced) {
            *advanced = local_advanced;
        }

        return Status::OK();
    }

    static Validated<T> defaultConstruct() {
        return Validated<T>();
    }
};

}  // namespace mongo
