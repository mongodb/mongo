// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/modules.h"

namespace mongo::sbe::value {

struct DoubleDoubleSumState {
    static DoubleDoubleSumState read(const Array& array);

    void add(TypeTags tag, Value val);
    void write(Array& array) const;
    TagValueOwned finalize() const;

    TypeTags nonDecimalTag = TypeTags::NumberInt32;
    bool hasDecimal = false;
    double sum = 0;
    double addend = 0;
    Decimal128 decimalTotal;
};

}  // namespace mongo::sbe::value
