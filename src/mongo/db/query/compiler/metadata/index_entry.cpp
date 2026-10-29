// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/metadata/index_entry.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/util/assert_util.h"

#include <string_view>


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
        sb << " filterExpr: " << filterExpr->debugString();
    }

    if (!infoObj.isEmpty()) {
        sb << " io: " << infoObj;
    }

    return sb.str();
}

bool IndexEntry::pathHasMultikeyComponent(std::string_view indexedField) const {
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

BSONElement IndexEntry::getWildcardField() const {
    uassert(7246601, "The index is not a wildcard index", type == IndexType::INDEX_WILDCARD);

    BSONObjIterator it(keyPattern);
    BSONElement wildcardElt = it.next();
    for (size_t i = 0; i < wildcardFieldPos; ++i) {
        tassert(11051935, "the wildcardFieldPos is larger than the keyPattern length", it.more());
        wildcardElt = it.next();
    }

    return wildcardElt;
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
