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

#include "mongo/platform/basic.h"

#include "mongo/db/query/index_entry.h"

#include "mongo/db/matcher/expression.h"

namespace mongo {

std::string IndexEntry::toString() const {
    StringBuilder sb;
    sb << "kp: " << keyPattern;

    if (multikey) {
        sb << " multikey";
    }

    if (sparse) {
        sb << " sparse";
    }

    if (unique) {
        sb << " unique";
    }

    sb << " name: '" << identifier << "'";

    if (filterExpr) {
        sb << " filterExpr: " << filterExpr->toString();
    }

    if (!infoObj.isEmpty()) {
        sb << " io: " << infoObj;
    }

    return sb.str();
}

bool IndexEntry::pathHasMultikeyComponent(StringData indexedField) const {
    if (multikeyPaths.empty()) {
        // The index has no path-level multikeyness metadata.
        return multikey;
    }

    size_t pos = 0;
    for (auto&& key : keyPattern) {
        if (key.fieldNameStringData() == indexedField) {
            return !multikeyPaths[pos].empty();
        }
        ++pos;
    }

    MONGO_UNREACHABLE;
}

std::ostream& operator<<(std::ostream& stream, const IndexEntry::Identifier& ident) {
    stream << ident.toString();
    return stream;
}

StringBuilder& operator<<(StringBuilder& builder, const IndexEntry::Identifier& ident) {
    builder << ident.toString();
    return builder;
}

}  // namespace mongo
