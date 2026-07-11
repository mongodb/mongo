// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/bson/bsonmisc.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"

#include <algorithm>

namespace mongo {

bool fieldsMatch(const BSONObj& a, const BSONObj& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](auto&& ae, auto&& be) {
        return ae.fieldNameStringData() == be.fieldNameStringData();
    });
}

}  // namespace mongo
