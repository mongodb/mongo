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

#pragma once

#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"

/**
 * This provides helpers for creating oplog entries. To create a $v: 1 modifier-style oplog
 * entry, a LogBuilder must be used instead.
 */
namespace mongo::update_oplog_entry {
static inline constexpr StringData kDiffObjectFieldName = "diff"_sd;

constexpr size_t kSizeOfDeltaOplogEntryMetadata = 15;

/**
 * Represents the "type" of an update oplog entry. For historical reasons these do not always
 * correspond to the value in the $v field of the entry. To determine the type of an update oplog
 * entry use extractUpdateType().
 *
 * Pipeline updates are not represented here because they are always logged with replacement
 * entries or $v:2 delta entries.
 */
enum class UpdateType {
    kReplacement,
    kV1Modifier,
    kV2Delta,
};

/**
 * Used for indicating whether a field was removed or not. 'kUnknown' represents the case where it
 * cannot be determined whether a field was removed by just examining the update.
 */
enum class FieldRemovedStatus { kFieldRemoved, kFieldNotRemoved, kUnknown };

/**
 * Given a diff, produce the contents for the 'o' field of a $v: 2 delta-style oplog entry.
 */
BSONObj makeDeltaOplogEntry(const doc_diff::Diff& diff);

/**
 * Produce the contents of the 'o' field of a replacement style oplog entry.
 */
inline BSONObj makeReplacementOplogEntry(const BSONObj& replacement) {
    return replacement;
}

/**
 * Given the 'o' field of an update oplog entry, determine its type. If the type cannot be
 * determined, boost::none is returned.
 */
UpdateType extractUpdateType(const BSONObj& oField);

/**
 * Given the 'o' field of an update oplog entry, this function will attempt to recover the new
 * value for the top-level field provided in 'fieldName'. Will return:
 *
 * -An EOO BSONElement if the field was deleted as part of the update or if the field's new value
 * cannot be recovered from the update object. The latter case can happen when a field is not
 * modified by the update at all, or when the field is an object and one of its subfields is
 * modified.
 * -A BSONElement with field's new value if it was added or set to a new value as part of the
 * update.
 *
 * 'fieldName' *MUST* be a top-level field. It may not contain dots.
 *
 * It is a programming error to call this function with a value for 'updateObj' that is not a valid
 * update.
 */
BSONElement extractNewValueForField(const BSONObj& updateObj, StringData fieldName);

/**
 * Given the 'o' field of an update oplog entry document, this function will determine whether the
 * given field was deleted by the update. 'fieldName' must be a top-level field, and may not
 * include any dots.
 *
 * If this function is passed a replacement style update it may return 'kUnknown' because it is
 * impossible to determine whether a field was deleted during a replacement update without
 * examining the pre-image.
 *
 * It is a programming error to call this function with a value for 'updateObj' that is not a valid
 * update.
 */
FieldRemovedStatus isFieldRemovedByUpdate(const BSONObj& updateObj, StringData fieldName);
}  // namespace mongo::update_oplog_entry
