/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/update/update_oplog_entry_serialization.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo::update_oplog_entry {
BSONObj makeDeltaOplogEntry(const doc_diff::Diff& diff) {
    BSONObjBuilder builder;
    builder.append("$v", static_cast<int>(UpdateOplogEntryVersion::kDeltaV2));
    builder.append(kDiffObjectFieldName, diff);
    return builder.obj();
}

boost::optional<BSONObj> extractDiffFromOplogEntry(const BSONObj& opLog) {
    auto version = opLog["$v"];
    if (version.ok() &&
        version.numberInt() == static_cast<int>(UpdateOplogEntryVersion::kDeltaV2)) {
        auto diff = opLog[kDiffObjectFieldName];
        if (diff.type() == BSONType::object) {
            return diff.embeddedObject();
        }
    }
    return {};
}

namespace {
BSONElement extractNewValueForFieldFromV2Entry(const BSONObj& oField, StringData fieldName) {
    auto diffField = oField[kDiffObjectFieldName];

    // Every $v:2 oplog entry should have a 'diff' field that is an object.
    invariant(diffField.type() == BSONType::object);
    doc_diff::DocumentDiffReader reader(diffField.embeddedObject());

    boost::optional<BSONElement> nextMod;
    while ((nextMod = reader.nextUpdate()) || (nextMod = reader.nextInsert())) {
        if (nextMod->fieldNameStringData() == fieldName) {
            return *nextMod;
        }
    }

    // The field may appear in the "delete" section or not at all.
    return BSONElement();
}

FieldRemovedStatus isFieldRemovedByV2Update(const BSONObj& oField, StringData fieldName) {
    auto diffField = oField[kDiffObjectFieldName];

    // Every $v:2 oplog entry should have a 'diff' field that is an object.
    invariant(diffField.type() == BSONType::object);
    doc_diff::DocumentDiffReader reader(diffField.embeddedObject());

    boost::optional<StringData> nextDelete;
    while ((nextDelete = reader.nextDelete())) {
        if (*nextDelete == fieldName) {
            return FieldRemovedStatus::kFieldRemoved;
        }
    }
    return FieldRemovedStatus::kFieldNotRemoved;
}
}  // namespace

UpdateType extractUpdateType(const BSONObj& updateDocument) {
    // For an update oplog entry, a replacement type should always have _id field.
    if (updateDocument["_id"]) {
        return UpdateType::kReplacement;
    }

    // Use the "$v" field to determine which type of update this is. Note $v:1 updates were allowed
    // to omit the $v field so that case must be handled carefully.
    auto vElt = updateDocument[kUpdateOplogEntryVersionFieldName];

    if (vElt.ok() && vElt.numberInt() == static_cast<int>(UpdateOplogEntryVersion::kDeltaV2)) {
        return UpdateType::kV2Delta;
    }

    // Unrecognized oplog entry version.
    tasserted(6448500, str::stream() << "Unsupported or missing oplog version, " << vElt);
}

BSONElement extractNewValueForField(const BSONObj& oField, StringData fieldName) {
    invariant(fieldName.find('.') == std::string::npos, "field name cannot contain dots");

    auto type = extractUpdateType(oField);

    if (type == UpdateType::kV2Delta) {
        return extractNewValueForFieldFromV2Entry(oField, fieldName);
    } else if (type == UpdateType::kReplacement) {
        return oField[fieldName];
    }

    // Clearly an unsupported format.
    MONGO_UNREACHABLE;
}

FieldRemovedStatus isFieldRemovedByUpdate(const BSONObj& oField, StringData fieldName) {
    invariant(fieldName.find('.') == std::string::npos, "field name cannot contain dots");

    auto type = extractUpdateType(oField);

    if (type == UpdateType::kV2Delta) {
        return isFieldRemovedByV2Update(oField, fieldName);
    } else if (type == UpdateType::kReplacement) {
        // The field was definitely *not* removed if it's still in the post image. Otherwise,
        // we cannot tell if it was removed without looking at the pre image.
        return oField.hasField(fieldName) ? FieldRemovedStatus::kFieldNotRemoved
                                          : FieldRemovedStatus::kUnknown;
    }
    MONGO_UNREACHABLE;
}
}  // namespace mongo::update_oplog_entry
