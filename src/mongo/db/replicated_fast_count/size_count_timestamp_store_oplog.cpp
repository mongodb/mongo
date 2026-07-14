// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_timestamp_store_oplog.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/container_oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/storage/ident.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo::replicated_fast_count {
namespace {

// Returns true if 'opType'/'container' describe a container insert ('ci') or update ('cu')
// targeting the fast count timestamp store ident.
bool isTimestampStoreContainerOp(repl::OpTypeEnum opType,
                                 const boost::optional<std::string_view>& container) {
    const bool isContainerWrite = opType == repl::OpTypeEnum::kContainerInsert ||
        opType == repl::OpTypeEnum::kContainerUpdate;
    return isContainerWrite && container && *container == ident::kFastCountMetadataStoreTimestamps;
}

// Extracts the 'valid-as-of' timestamp carried in the 'o' field of a timestamp store container op,
// tripping a tassert if that value is malformed.
Timestamp validAsOfFromTimestampStoreObject(const BSONObj& object) {
    // The 'o' field carries the on-disk value in its 'v' BinData. Parse it through the container
    // oplog entry IDL; 'ContainerInsertOplogEntryO' covers both 'ci' and 'cu' (the update-only
    // '$v' field is ignored because the struct is non-strict).
    const auto containerO = repl::ContainerInsertOplogEntryO::parse(
        object, IDLParserContext("FastCountTimestampStoreContainerOp"));
    uassert(
        13064102, "Container object must have a value field", containerO.getValue().has_value());
    const auto valueBytes = containerO.getValue()->data();
    const BSONObj value(valueBytes.data());

    const auto validAsOfElem = value[kValidAsOfKey];
    tassert(12984805,
            fmt::format("Fast count timestamp store write is missing a '{}' timestamp; value: {}",
                        kValidAsOfKey,
                        redact(value).toString()),
            validAsOfElem.type() == BSONType::timestamp);
    return validAsOfElem.timestamp();
}

// Handles an oplog entry that is not an applyOps command: the entry must itself be the timestamp
// store 'ci'/'cu' write. Asserts that shape and returns its 'valid-as-of' timestamp.
Timestamp validAsOfFromTopLevelOp(const repl::DurableOplogEntry& entry) {
    tassert(12984804,
            str::stream() << "Oplog entry is not a fast count timestamp store write: "
                          << redact(entry.getRaw()),
            isTimestampStoreContainerOp(entry.getOpType(), entry.getContainer()));
    return validAsOfFromTimestampStoreObject(entry.getObject());
}

// Handles an applyOps command entry: exactly one of its inner operations is the timestamp store
// write. Filters the inner operations on the container ident and returns that op's 'valid-as-of'
// timestamp.
Timestamp validAsOfFromApplyOps(const repl::DurableOplogEntry& entry) {
    const auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(entry.getObject());

    boost::optional<Timestamp> result;
    for (const auto& innerOpObj : applyOpsInfo.getOperations()) {
        const auto innerOp = repl::DurableReplOperation::parse(
            innerOpObj, IDLParserContext("FastCountApplyOpsInnerOp"));
        if (!isTimestampStoreContainerOp(innerOp.getOpType(), innerOp.getContainer())) {
            continue;
        }
        // A single applyOps batch must never carry more than one timestamp store write.
        tassert(
            12984803,
            str::stream() << "applyOps oplog entry contains multiple fast count timestamp store "
                             "writes, which should not be possible: "
                          << redact(entry.getRaw()),
            !result);
        result = validAsOfFromTimestampStoreObject(innerOp.getObject());
    }

    tassert(12984802,
            str::stream() << "applyOps oplog entry contains no fast count timestamp store write: "
                          << redact(entry.getRaw()),
            result.has_value());
    return *result;
}

}  // namespace

Timestamp getTimestampStoreValidAsOfFromOplogEntry(const BSONObj& oplogEntry) {
    auto swEntry = repl::DurableOplogEntry::parse(oplogEntry);
    tassert(12984806,
            str::stream() << "Failed to parse fast count timestamp store oplog entry: "
                          << swEntry.getStatus(),
            swEntry.isOK());
    const auto& entry = swEntry.getValue();

    // A timestamp store write reaches the oplog in one of two shapes: as a top-level container op,
    // or as a single container op nested inside an applyOps command. The two shapes have different
    // contracts, so dispatch to a dedicated helper for each.
    if (entry.getCommandType() != repl::CommandTypeEnum::kApplyOps) {
        return validAsOfFromTopLevelOp(entry);
    }
    return validAsOfFromApplyOps(entry);
}

BSONObj fastCountValidAsOfScanFilter() {
    const std::string timestampsIdent{ident::kFastCountMetadataStoreTimestamps};
    // Match the ident either as a top-level container op or nested in an applyOps array.
    return BSON("$or" << BSON_ARRAY(BSON("container" << timestampsIdent)
                                    << BSON("o.applyOps.container" << timestampsIdent)));
}

}  // namespace mongo::replicated_fast_count
