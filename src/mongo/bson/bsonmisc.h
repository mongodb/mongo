// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class BSONElementCmpWithoutField {
public:
    /**
     * If 'stringComparator' is null, the default binary comparator will be used for comparing
     * string elements.  A custom string comparator may be provided, but it must outlive the
     * constructed BSONElementCmpWithoutField.
     */
    BSONElementCmpWithoutField(const StringDataComparator* stringComparator = nullptr)
        : _stringComparator(stringComparator) {}

    bool operator()(const BSONElement& l, const BSONElement& r) const {
        return l.woCompare(r, false, _stringComparator) < 0;
    }

private:
    const StringDataComparator* _stringComparator;
};

// considers order
bool fieldsMatch(const BSONObj& lhs, const BSONObj& rhs);
}  // namespace mongo
