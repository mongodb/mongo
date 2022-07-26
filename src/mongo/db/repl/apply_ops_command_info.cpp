/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/repl/apply_ops_command_info.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace repl {

namespace {

/**
 * Return true iff the applyOpsCmd can be executed in a single WriteUnitOfWork.
 */
bool _parseAreOpsCrudOnly(const BSONObj& applyOpCmd) {
    for (const auto& elem : applyOpCmd.firstElement().Obj()) {
        const char* opType = elem.Obj().getStringField("op").rawData();

        // All atomic ops have an opType of length 1.
        if (opType[0] == '\0' || opType[1] != '\0')
            return false;

        // Only consider CRUD operations.
        switch (*opType) {
            case 'd':
            case 'n':
            case 'u':
                break;
            case 'i':
                break;
            // Fallthrough.
            default:
                return false;
        }
    }

    return true;
}

}  // namespace

// static
ApplyOpsCommandInfo ApplyOpsCommandInfo::parse(const BSONObj& applyOpCmd) {
    try {
        return ApplyOpsCommandInfo(applyOpCmd);
    } catch (DBException& ex) {
        ex.addContext(str::stream() << "Failed to parse applyOps command: " << redact(applyOpCmd));
        throw;
    }
}

bool ApplyOpsCommandInfo::areOpsCrudOnly() const {
    return _areOpsCrudOnly;
}

bool ApplyOpsCommandInfo::isAtomic() const {
    return getAllowAtomic() && areOpsCrudOnly();
}

ApplyOpsCommandInfo::ApplyOpsCommandInfo(const BSONObj& applyOpCmd)
    : _areOpsCrudOnly(_parseAreOpsCrudOnly(applyOpCmd)) {
    parseProtected(IDLParserContext("applyOps"), applyOpCmd);

    if (getPreCondition()) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot use preCondition with {allowAtomic: false}",
                getAllowAtomic());
        uassert(ErrorCodes::InvalidOptions,
                "Cannot use preCondition when operations include commands.",
                areOpsCrudOnly());
    }
}

// static
std::vector<OplogEntry> ApplyOps::extractOperations(const OplogEntry& applyOpsOplogEntry) {
    std::vector<OplogEntry> result;
    extractOperationsTo(applyOpsOplogEntry, applyOpsOplogEntry.getEntry().toBSON(), &result);
    return result;
}

// static
void ApplyOps::extractOperationsTo(const OplogEntry& applyOpsOplogEntry,
                                   const BSONObj& topLevelDoc,
                                   std::vector<OplogEntry>* operations) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "ApplyOps::extractOperations(): not a command: "
                          << redact(applyOpsOplogEntry.toBSONForLogging()),
            applyOpsOplogEntry.isCommand());

    uassert(ErrorCodes::CommandNotSupported,
            str::stream() << "ApplyOps::extractOperations(): not applyOps command: "
                          << redact(applyOpsOplogEntry.toBSONForLogging()),
            OplogEntry::CommandType::kApplyOps == applyOpsOplogEntry.getCommandType());

    auto cmdObj = applyOpsOplogEntry.getOperationToApply();
    auto info = ApplyOpsCommandInfo::parse(cmdObj);
    auto operationDocs = info.getOperations();
    bool alwaysUpsert = info.getAlwaysUpsert() && !applyOpsOplogEntry.getTxnNumber();

    uint64_t applyOpsIdx{0};
    for (const auto& operationDoc : operationDocs) {
        // Make sure that the inner ops are not malformed or over-specified.
        ReplOperation::parse(IDLParserContext("extractOperations"), operationDoc);

        BSONObjBuilder builder(operationDoc);

        // Oplog entries can have an oddly-named "b" field for "upsert". MongoDB stopped creating
        // such entries in 4.0, but we can use the "b" field for the extracted entry here.
        if (alwaysUpsert && !operationDoc.hasField("b")) {
            builder.append("b", true);
        }

        builder.appendElementsUnique(topLevelDoc);
        auto operation = builder.obj();

        operations->emplace_back(operation);

        // Preserve index of operation in the "applyOps" oplog entry, timestamp, and wall clock time
        // of the "applyOps" entry.
        auto& lastOperation = operations->back();
        lastOperation.setApplyOpsIndex(applyOpsIdx);
        lastOperation.setApplyOpsTimestamp(applyOpsOplogEntry.getTimestamp());
        lastOperation.setApplyOpsWallClockTime(applyOpsOplogEntry.getWallClockTime());
        ++applyOpsIdx;
    }
}

}  // namespace repl
}  // namespace mongo
