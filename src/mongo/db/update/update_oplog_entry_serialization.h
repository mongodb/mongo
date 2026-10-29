// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

#include <boost/optional/optional.hpp>

/**
 * This provides helpers for creating oplog entries.
 */
[[MONGO_MOD_PUBLIC]];
namespace mongo::update_oplog_entry {
using namespace std::literals::string_view_literals;
static inline constexpr std::string_view kDiffObjectFieldName = "diff"sv;

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
 *
 * If 'replacement' contains an _id field that is not already the first field, this returns a copy
 * with _id moved to the front so that the oplog entry matches the on-disk field order produced by
 * fixDocumentForInsert(). If _id is already first (or absent), 'replacement' is returned unchanged.
 */
BSONObj makeReplacementOplogEntry(const BSONObj& replacement);

/**
 * Given the 'o' field of an update oplog entry, determine its type. Throws if the object is not of
 * the expected format.
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
BSONElement extractNewValueForField(const BSONObj& updateObj, std::string_view fieldName);

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
FieldRemovedStatus isFieldRemovedByUpdate(const BSONObj& updateObj, std::string_view fieldName);
}  // namespace mongo::update_oplog_entry
