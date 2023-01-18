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

#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/str.h"

namespace mongo {

OrderedPathSet DepsTracker::simplifyDependencies(OrderedPathSet dependencies,
                                                 TruncateToRootLevel truncateToRootLevel) {
    // The key operation here is folding dependencies into ancestor dependencies, wherever possible.
    // This is assisted by a special sort in OrderedPathSet that treats '.'
    // as the first char and thus places parent paths directly before their children.
    OrderedPathSet returnSet;
    std::string last;
    for (const auto& path : dependencies) {
        if (!last.empty() && str::startsWith(path, last)) {
            // We are including a parent of this field, so we can skip this field.
            continue;
        }

        // Check that the field requested is a valid field name in the agg language. This
        // constructor will throw if it isn't.
        FieldPath fp(path);

        if (truncateToRootLevel == TruncateToRootLevel::yes) {
            last = fp.front().toString() + '.';
            returnSet.insert(fp.front().toString());
        } else {
            last = path + '.';
            returnSet.insert(path);
        }
    }
    return returnSet;
}

BSONObj DepsTracker::toProjectionWithoutMetadata(
    TruncateToRootLevel truncationBehavior /*= TruncateToRootLevel::no*/) const {
    BSONObjBuilder bb;

    if (needWholeDocument) {
        return bb.obj();
    }

    if (fields.empty()) {
        // We need no user-level fields (as we would if this was logically a count). Since there is
        // no way of expressing a projection that indicates no depencies, we return an empty
        // projection.
        return bb.obj();
    }

    // Create a projection from the simplified dependencies (absorbing descendants into parents).
    // For example, the dependencies ["a.b", "a.b.c.g", "c", "c.d", "f"] would be
    // minimally covered by the projection {"a.b": 1, "c": 1, "f": 1}.
    bool idSpecified = false;
    for (auto path : simplifyDependencies(fields, truncationBehavior)) {
        // Remember if _id was specified.  If not, we'll later explicitly add {_id: 0}
        if (str::startsWith(path, "_id") && (path.size() == 3 || path[3] == '.')) {
            idSpecified = true;
        }
        bb.append(path, 1);
    }

    if (!idSpecified) {
        bb.append("_id", 0);
    }

    return bb.obj();
}

void DepsTracker::setNeedsMetadata(DocumentMetadataFields::MetaType type, bool required) {
    uassert(40218,
            str::stream() << "query requires " << type << " metadata, but it is not available",
            !required || !_unavailableMetadata[type]);

    // If the metadata type is not required, then it should not be recorded as a metadata
    // dependency.
    invariant(required || !_metadataDeps[type]);
    _metadataDeps[type] = required;
}

// Returns true if the lhs value should sort before the rhs, false otherwise.
bool PathComparator::operator()(const std::string& lhs, const std::string& rhs) const {
    constexpr char dot = '.';

    for (size_t pos = 0, len = std::min(lhs.size(), rhs.size()); pos < len; ++pos) {
        // Below, we explicitly choose unsigned char because the usual const char& returned by
        // operator[] is actually signed on x86 and will incorrectly order unicode characters.
        unsigned char lchar = lhs[pos], rchar = rhs[pos];
        if (lchar == rchar) {
            continue;
        }

        // Consider the path delimiter '.' as being less than all other characters, so that
        // paths sort directly before any paths they prefix and directly after any paths
        // which prefix them.
        if (lchar == dot) {
            return true;
        } else if (rchar == dot) {
            return false;
        }

        // Otherwise, default to normal character comparison.
        return lchar < rchar;
    }

    // If we get here, then we have reached the end of lhs and/or rhs and all of their path
    // segments up to this point match. If lhs is shorter than rhs, then lhs prefixes rhs
    // and should sort before it.
    return lhs.size() < rhs.size();
}

}  // namespace mongo
