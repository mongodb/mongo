/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstddef>
#include <cstring>
#include <limits>

#include "mongo/base/data_range.h"
#include "mongo/base/data_type.h"
#include "mongo/platform/endian.h"
#include "mongo/util/str.h"

namespace mongo {

class ConstDataRangeCursor : public ConstDataRange {
public:
    using ConstDataRange::ConstDataRange;
    ConstDataRangeCursor(ConstDataRange cdr) : ConstDataRange(cdr) {}

    Status advanceNoThrow(size_t advance) noexcept try {
        if (advance > length()) {
            return makeAdvanceStatus(advance);
        }

        _begin += advance;
        _debug_offset += advance;

        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }

    void advance(size_t advance) {
        uassertStatusOK(advanceNoThrow(advance));
    }

    template <typename T>
    Status skipNoThrow() noexcept {
        size_t advanced = 0;

        Status x = DataType::load<T>(nullptr, _begin, _end - _begin, &advanced, _debug_offset);

        if (x.isOK()) {
            _begin += advanced;
            _debug_offset += advanced;
        }

        return x;
    }

    template <typename T>
    void skip() {
        uassertStatusOK(skipNoThrow<T>());
    }

    template <typename T>
    Status readAndAdvanceNoThrow(T* t) noexcept {
        size_t advanced = 0;

        Status x = DataType::load(t, _begin, _end - _begin, &advanced, _debug_offset);

        if (x.isOK()) {
            _begin += advanced;
            _debug_offset += advanced;
        }

        return x;
    }

    template <typename T>
    void readAndAdvance(T* t) {
        return uassertStatusOK(readAndAdvanceNoThrow(t));
    }

    template <typename T>
    StatusWith<T> readAndAdvanceNoThrow() noexcept {
        T out(DataType::defaultConstruct<T>());
        Status x = readAndAdvanceNoThrow(&out);

        if (x.isOK()) {
            return StatusWith<T>(std::move(out));
        } else {
            return StatusWith<T>(std::move(x));
        }
    }

    template <typename T>
    T readAndAdvance() {
        return uassertStatusOK(readAndAdvanceNoThrow<T>());
    }

private:
    Status makeAdvanceStatus(size_t advance) const;
};

class DataRangeCursor : public DataRange {
public:
    using DataRange::DataRange;
    DataRangeCursor(DataRange range) : DataRange(range) {}

    operator ConstDataRangeCursor() const {
        return ConstDataRangeCursor(ConstDataRange(_begin, _end, _debug_offset));
    }

    Status advanceNoThrow(size_t advance) noexcept {
        if (advance > length()) {
            return makeAdvanceStatus(advance);
        }

        _begin += advance;
        _debug_offset += advance;

        return Status::OK();
    }

    void advance(size_t advance) {
        uassertStatusOK(advanceNoThrow(advance));
    }

    template <typename T>
    Status skipNoThrow() noexcept {
        size_t advanced = 0;

        Status x = DataType::load<T>(nullptr, _begin, _end - _begin, &advanced, _debug_offset);

        if (x.isOK()) {
            _begin += advanced;
            _debug_offset += advanced;
        }

        return x;
    }

    template <typename T>
    void skip() {
        uassertStatusOK(skipNoThrow<T>());
    }

    template <typename T>
    Status readAndAdvanceNoThrow(T* t) noexcept {
        size_t advanced = 0;

        Status x = DataType::load(t, _begin, _end - _begin, &advanced, _debug_offset);

        if (x.isOK()) {
            _begin += advanced;
            _debug_offset += advanced;
        }

        return x;
    }

    template <typename T>
    void readAndAdvance(T* t) {
        uassertStatusOK(readAndAdvanceNoThrow(t));
    }

    template <typename T>
    StatusWith<T> readAndAdvanceNoThrow() noexcept {
        T out(DataType::defaultConstruct<T>());
        Status x = readAndAdvanceNoThrow(&out);

        if (x.isOK()) {
            return StatusWith<T>(std::move(out));
        } else {
            return StatusWith<T>(std::move(x));
        }
    }

    template <typename T>
    T readAndAdvance() {
        return uassertStatusOK(readAndAdvanceNoThrow<T>());
    }

    template <typename T>
    Status writeAndAdvanceNoThrow(const T& value) noexcept {
        size_t advanced = 0;

        Status x = DataType::store(
            value, const_cast<char*>(_begin), _end - _begin, &advanced, _debug_offset);

        if (x.isOK()) {
            _begin += advanced;
            _debug_offset += advanced;
        }

        return x;
    }

    template <typename T>
    void writeAndAdvance(const T& value) {
        uassertStatusOK(writeAndAdvanceNoThrow(value));
    }

private:
    Status makeAdvanceStatus(size_t advance) const;
};

}  // namespace mongo
