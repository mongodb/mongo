// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/data_type.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/platform/endian.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

[[MONGO_MOD_PUBLIC]];

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

    /**
     * Return a ConstDataRange based on `splitPoint`
     * and advance the cursor past there.
     */
    template <typename ByteLike>
    ConstDataRange sliceAndAdvance(const ByteLike& splitPoint) {
        auto ret = slice(splitPoint);
        advance(ret.length());
        return ret;
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

    /**
     * Return a DataRange based on `splitPoint`
     * and advance the cursor past there.
     */
    template <typename ByteLike>
    DataRange sliceAndAdvance(const ByteLike& splitPoint) {
        auto ret = slice(splitPoint);
        advance(ret.length());
        return ret;
    }

private:
    Status makeAdvanceStatus(size_t advance) const;
};

}  // namespace mongo
