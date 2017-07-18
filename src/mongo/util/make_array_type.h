/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <array>

#pragma push_macro("MONGO_CONSTEXPR")
#undef MONGO_CONSTEXPR

#ifdef _MSC_VER
#define MONGO_CONSTEXPR
#else
#define MONGO_CONSTEXPR constexpr
#endif

namespace mongo {

/**
 * A synthetic std::array alike builder for making array's with unique types.
 */
template <typename T, size_t N, typename Tag>
struct MakeArrayType {
    using Array = std::array<T, N>;

    using value_type = typename Array::value_type;
    using size_type = typename Array::size_type;
    using difference_type = typename Array::difference_type;
    using reference = typename Array::reference;
    using const_reference = typename Array::const_reference;
    using pointer = typename Array::pointer;
    using const_pointer = typename Array::const_pointer;
    using iterator = typename Array::iterator;
    using const_iterator = typename Array::const_iterator;
    using reverse_iterator = typename Array::reverse_iterator;
    using const_reverse_iterator = typename Array::const_reverse_iterator;

    reference at(size_type pos) {
        return _data.at(pos);
    }

    const_reference at(size_type pos) const {
        return _data.at(pos);
    }

    reference operator[](size_type pos) {
        return _data[pos];
    }

    const_reference operator[](size_type pos) const {
        return _data[pos];
    }

    reference front() {
        return _data.front();
    }

    const_reference front() const {
        return _data.front();
    }

    reference back() {
        return _data.back();
    }

    const_reference back() const {
        return _data.back();
    }

    pointer data() {
        return _data.data();
    }

    const_pointer data() const {
        return _data.data();
    }

    MONGO_CONSTEXPR iterator begin() noexcept {
        return _data.begin();
    }

    MONGO_CONSTEXPR const_iterator begin() const noexcept {
        return _data.begin();
    }

    MONGO_CONSTEXPR const_iterator cbegin() const noexcept {
        return _data.begin();
    }

    MONGO_CONSTEXPR iterator end() noexcept {
        return _data.end();
    }

    MONGO_CONSTEXPR const_iterator end() const noexcept {
        return _data.end();
    }

    MONGO_CONSTEXPR const_iterator cend() const noexcept {
        return _data.end();
    }

    MONGO_CONSTEXPR reverse_iterator rbegin() noexcept {
        return _data.rbegin();
    }

    MONGO_CONSTEXPR const_reverse_iterator rbegin() const noexcept {
        return _data.rbegin();
    }

    MONGO_CONSTEXPR const_reverse_iterator crbegin() const noexcept {
        return _data.rbegin();
    }

    MONGO_CONSTEXPR reverse_iterator rend() noexcept {
        return _data.rend();
    }

    MONGO_CONSTEXPR const_reverse_iterator rend() const noexcept {
        return _data.rend();
    }

    MONGO_CONSTEXPR const_reverse_iterator crend() const noexcept {
        return _data.rend();
    }

    MONGO_CONSTEXPR bool empty() const noexcept {
        return _data.empty();
    }

    MONGO_CONSTEXPR size_t size() const noexcept {
        return _data.size();
    }

    MONGO_CONSTEXPR size_t max_size() const noexcept {
        return _data.max_size();
    }

    void fill(const T& value) {
        return _data.fill(value);
    }

    void swap(MakeArrayType& other) noexcept {
        return _data.swap(other._data);
    }

    friend bool operator==(const MakeArrayType& lhs, const MakeArrayType& rhs) {
        return lhs._data == rhs._data;
    }

    friend bool operator!=(const MakeArrayType& lhs, const MakeArrayType& rhs) {
        return lhs._data != rhs._data;
    }

    friend bool operator<(const MakeArrayType& lhs, const MakeArrayType& rhs) {
        return lhs._data < rhs._data;
    }

    friend bool operator>(const MakeArrayType& lhs, const MakeArrayType& rhs) {
        return lhs._data > rhs._data;
    }

    friend bool operator<=(const MakeArrayType& lhs, const MakeArrayType& rhs) {
        return lhs._data <= rhs._data;
    }

    friend bool operator>=(const MakeArrayType& lhs, const MakeArrayType& rhs) {
        return lhs._data >= rhs._data;
    }

    friend MONGO_CONSTEXPR T& get(MakeArrayType& mat) noexcept {
        return get(mat._data);
    }

    friend MONGO_CONSTEXPR T&& get(MakeArrayType&& mat) noexcept {
        return get(mat._data);
    }

    friend MONGO_CONSTEXPR const T& get(const MakeArrayType& mat) noexcept {
        return get(mat._data);
    }

    friend MONGO_CONSTEXPR const T&& get(const MakeArrayType&& mat) noexcept {
        return get(mat._data);
    }

    friend void swap(MakeArrayType& lhs, MakeArrayType& rhs) noexcept {
        using std::swap;
        return swap(lhs._data, rhs._data);
    }

    Array _data;
};

}  // namespace mongo

#undef MONGO_CONSTEXPR
#pragma pop_macro("MONGO_CONSTEXPR")
