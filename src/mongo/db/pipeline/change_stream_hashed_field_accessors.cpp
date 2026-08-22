// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_hashed_field_accessors.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/repl/oplog_entry.h"

namespace mongo::change_stream {

const HashedFieldName HashedFieldAccessors::kOpType =
    FieldNameHasher().hashedFieldName(repl::OplogEntry::kOpTypeFieldName);
const HashedFieldName HashedFieldAccessors::kTimestamp =
    FieldNameHasher().hashedFieldName(repl::OplogEntry::kTimestampFieldName);
const HashedFieldName HashedFieldAccessors::kNss =
    FieldNameHasher().hashedFieldName(repl::OplogEntry::kNssFieldName);
const HashedFieldName HashedFieldAccessors::kUuid =
    FieldNameHasher().hashedFieldName(repl::OplogEntry::kUuidFieldName);
const HashedFieldName HashedFieldAccessors::kObject =
    FieldNameHasher().hashedFieldName(repl::OplogEntry::kObjectFieldName);
const HashedFieldName HashedFieldAccessors::kObject2 =
    FieldNameHasher().hashedFieldName(repl::OplogEntry::kObject2FieldName);
const HashedFieldName HashedFieldAccessors::kWallClockTime =
    FieldNameHasher().hashedFieldName(repl::OplogEntry::kWallClockTimeFieldName);
const HashedFieldName HashedFieldAccessors::kFromMigrate =
    FieldNameHasher().hashedFieldName(repl::OplogEntry::kFromMigrateFieldName);
const HashedFieldName HashedFieldAccessors::kTxnOpIndex =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kTxnOpIndexField);
const HashedFieldName HashedFieldAccessors::kLsid =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kLsidField);
const HashedFieldName HashedFieldAccessors::kTxnNumber =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kTxnNumberField);
const HashedFieldName HashedFieldAccessors::kCommitTimestamp =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kCommitTimestampField);
const HashedFieldName HashedFieldAccessors::kApplyOpsIndex =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kApplyOpsIndexField);
const HashedFieldName HashedFieldAccessors::kApplyOpsEntryTs =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kApplyOpsTsField);
const HashedFieldName HashedFieldAccessors::kOperationType =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kOperationTypeField);
const HashedFieldName HashedFieldAccessors::kDocumentKey =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kDocumentKeyField);
const HashedFieldName HashedFieldAccessors::kNamespace =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStream::kNamespaceField);
const HashedFieldName HashedFieldAccessors::kPreImageId =
    FieldNameHasher().hashedFieldName(DocumentSourceChangeStreamAddPreImage::kPreImageIdFieldName);
const HashedFieldName HashedFieldAccessors::kPreImage =
    FieldNameHasher().hashedFieldName(ChangeStreamPreImage::kPreImageFieldName);

}  // namespace mongo::change_stream
