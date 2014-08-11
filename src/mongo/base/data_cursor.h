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

#include <cstddef>
#include <cstring>

#include "mongo/base/data_view.h"
#include "mongo/platform/endian.h"

namespace mongo {

    template <class T>
    struct CursorMethods : public T {

        CursorMethods(typename T::bytes_type bytes) : T(bytes) {}

        T operator+(std::size_t s) const {
            return this->T::view() + s;
        }

        T& operator+=(std::size_t s) {
            *this = this->T::view() + s;
            return *this;
        }

        T operator-(std::size_t s) const {
            return this->T::view() - s;
        }

        T& operator-=(std::size_t s) {
            *this = this->T::view() - s;
            return *this;
        }

        T& operator++() {
            return this->operator+=(1);
        }

        T operator++(int) {
            T tmp = *this;
            operator++();
            return tmp;
        }

        T& operator--() {
            return this->operator-=(1);
        }

        T operator--(int) {
            T tmp = *this;
            operator--();
            return tmp;
        }

        template <typename U>
        void skip() {
            *this = this->T::view() + sizeof(U);
        }

        template <typename U>
        U readNativeAndAdvance() {
            U out = this->T::template readNative<U>();
            this->skip<U>();
            return out;
        }

        template <typename U>
        U readLEAndAdvance() {
            return littleToNative(readNativeAndAdvance<U>());
        }

        template <typename U>
        U readBEAndAdvance() {
            return bigToNative(readNativeAndAdvance<U>());
        }

    };

    class ConstDataCursor : public CursorMethods<ConstDataView> {
    public:

        ConstDataCursor(ConstDataView::bytes_type bytes) : CursorMethods<ConstDataView>(bytes) {}
    };

    class DataCursor : public CursorMethods<DataView> {
    public:

        DataCursor(DataView::bytes_type bytes) : CursorMethods<DataView>(bytes) {}

        template <typename T>
        void writeNativeAndAdvance(const T& value) {
            this->writeNative(value);
            this->skip<T>();
        }

        template <typename T>
        void writeLEAndAdvance(const T& value) {
            return writeNativeAndAdvance(nativeToLittle(value));
        }

        template <typename T>
        void writeBEAndAdvance(const T& value) {
            return writeNativeAndAdvance(nativeToBig(value));
        }

        operator ConstDataCursor() const {
            return view();
        }
    };

} // namespace mongo
