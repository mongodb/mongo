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

#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"

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
        if (diff.type() == BSONType::Object) {
            return diff.embeddedObject();
        }
    }
    return {};
}

namespace {
BSONElement extractNewValueForFieldFromV1Entry(const BSONObj& oField, StringData fieldName) {
    // Check the '$set' section.
    auto setElt = oField["$set"];
    if (setElt.ok()) {
        // The $set field in a $v:1 entry should always be an object.
        invariant(setElt.type() == BSONType::Object);
        return setElt.embeddedObject()[fieldName];
    }

    // The field is either in the $unset section, or was not modified at all.
    return BSONElement();
}

BSONElement extractNewValueForFieldFromV2Entry(const BSONObj& oField, StringData fieldName) {
    auto diffField = oField[kDiffObjectFieldName];

    // Every $v:2 oplog entry should have a 'diff' field that is an object.
    invariant(diffField.type() == BSONType::Object);
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

FieldRemovedStatus isFieldRemovedByV1Update(const BSONObj& oField, StringData fieldName) {
    auto unsetElt = oField["$unset"];
    if (unsetElt.ok()) {
        invariant(unsetElt.type() == BSONType::Object);
        if (unsetElt.embeddedObject()[fieldName].ok()) {
            return FieldRemovedStatus::kFieldRemoved;
        }
    }
    return FieldRemovedStatus::kFieldNotRemoved;
}

FieldRemovedStatus isFieldRemovedByV2Update(const BSONObj& oField, StringData fieldName) {
    auto diffField = oField[kDiffObjectFieldName];

    // Every $v:2 oplog entry should have a 'diff' field that is an object.
    invariant(diffField.type() == BSONType::Object);
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

    if (!vElt.ok()) {
        // We're dealing with a $v:1 entry if the first field name starts with a '$' and there is no
        // $v field.
        if (updateDocument.firstElementFieldNameStringData().startsWith("$")) {
            return UpdateType::kV1Modifier;
        }
    } else if (vElt.numberInt() == static_cast<int>(UpdateOplogEntryVersion::kUpdateNodeV1)) {
        return UpdateType::kV1Modifier;
    } else if (vElt.numberInt() == static_cast<int>(UpdateOplogEntryVersion::kDeltaV2)) {
        return UpdateType::kV2Delta;
    }

    // Unrecognized oplog entry version.
    MONGO_UNREACHABLE_TASSERT(6448500);
}

BSONElement extractNewValueForField(const BSONObj& oField, StringData fieldName) {
    invariant(fieldName.find('.') == std::string::npos, "field name cannot contain dots");

    auto type = extractUpdateType(oField);

    if (type == UpdateType::kV1Modifier) {
        return extractNewValueForFieldFromV1Entry(oField, fieldName);
    } else if (type == UpdateType::kV2Delta) {
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

    if (type == UpdateType::kV1Modifier) {
        return isFieldRemovedByV1Update(oField, fieldName);
    } else if (type == UpdateType::kV2Delta) {
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
