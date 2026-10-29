// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

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
