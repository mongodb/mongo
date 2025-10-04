/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


namespace mongo::sbe {

class PrintOptions {
public:
    static constexpr size_t kDefaultStringMaxDisplayLength = 160;
    static constexpr size_t kDefaultBinDataMaxDisplayLength = 80;
    static constexpr size_t kDefaultArrayObjectOrNestingMaxDepth = 10;
    static constexpr size_t kDefaultUseTagForAmbiguousValues = false;
    static constexpr size_t kNormalizeOutput = false;

    size_t stringMaxDisplayLength() const {
        return _stringMaxDisplayLength;
    }

    PrintOptions& stringMaxDisplayLength(size_t value) {
        _stringMaxDisplayLength = value;
        return *this;
    }

    size_t binDataMaxDisplayLength() const {
        return _binDataMaxDisplayLength;
    }

    PrintOptions& binDataMaxDisplayLength(size_t value) {
        _binDataMaxDisplayLength = value;
        return *this;
    }

    size_t arrayObjectOrNestingMaxDepth() const {
        return _arrayObjectOrNestingMaxDepth;
    }

    PrintOptions& arrayObjectOrNestingMaxDepth(size_t value) {
        _arrayObjectOrNestingMaxDepth = value;
        return *this;
    }

    bool useTagForAmbiguousValues() const {
        return _useTagForAmbiguousValues;
    }

    PrintOptions& useTagForAmbiguousValues(bool value) {
        _useTagForAmbiguousValues = value;
        return *this;
    }

    bool normalizeOutput() const {
        return _normalizeOutput;
    }

    /**
     * Sets whether the output should be normalized. A normalized output will be
     * stable, determinisitc and platform independent.
     */
    PrintOptions& normalizeOutput(bool value) {
        _normalizeOutput = value;
        return *this;
    }

private:
    size_t _stringMaxDisplayLength = kDefaultStringMaxDisplayLength;
    size_t _binDataMaxDisplayLength = kDefaultBinDataMaxDisplayLength;
    size_t _arrayObjectOrNestingMaxDepth = kDefaultArrayObjectOrNestingMaxDepth;
    bool _useTagForAmbiguousValues = kDefaultUseTagForAmbiguousValues;
    bool _normalizeOutput = kNormalizeOutput;
};

}  // namespace mongo::sbe
