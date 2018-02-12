/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/ops/write_ops_retryability.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/find_and_modify_result.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/logger/redaction.h"

namespace mongo {
namespace {

/**
 * Validates that the request is retry-compatible with the operation that occurred.
 * In the case of nested oplog entry where the correct links are in the top level
 * oplog, oplogWithCorrectLinks can be used to specify the outer oplog.
 */
void validateFindAndModifyRetryability(const FindAndModifyRequest& request,
                                       const repl::OplogEntry& oplogEntry,
                                       const repl::OplogEntry& oplogWithCorrectLinks) {
    auto opType = oplogEntry.getOpType();
    auto ts = oplogEntry.getTimestamp();

    if (opType == repl::OpTypeEnum::kDelete) {
        uassert(
            40606,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType())
                          << ", oplogTs: "
                          << ts.toString()
                          << ", oplog: "
                          << redact(oplogEntry.toBSON()),
            request.isRemove());
        uassert(40607,
                str::stream() << "No pre-image available for findAndModify retry request:"
                              << redact(request.toBSON()),
                oplogWithCorrectLinks.getPreImageOpTime());
    } else if (opType == repl::OpTypeEnum::kInsert) {
        uassert(
            40608,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType())
                          << ", oplogTs: "
                          << ts.toString()
                          << ", oplog: "
                          << redact(oplogEntry.toBSON()),
            request.isUpsert());
    } else {
        uassert(
            40609,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType())
                          << ", oplogTs: "
                          << ts.toString()
                          << ", oplog: "
                          << redact(oplogEntry.toBSON()),
            opType == repl::OpTypeEnum::kUpdate);

        if (request.shouldReturnNew()) {
            uassert(40611,
                    str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                                  << " wants the document after update returned, but only before "
                                     "update document is stored, oplogTs: "
                                  << ts.toString()
                                  << ", oplog: "
                                  << redact(oplogEntry.toBSON()),
                    oplogWithCorrectLinks.getPostImageOpTime());
        } else {
            uassert(40612,
                    str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                                  << " wants the document before update returned, but only after "
                                     "update document is stored, oplogTs: "
                                  << ts.toString()
                                  << ", oplog: "
                                  << redact(oplogEntry.toBSON()),
                    oplogWithCorrectLinks.getPreImageOpTime());
        }
    }
}

/**
 * Extracts either the pre or post image (cannot be both) of the findAndModify operation from the
 * oplog.
 */
BSONObj extractPreOrPostImage(OperationContext* opCtx, const repl::OplogEntry& oplog) {
    invariant(oplog.getPreImageOpTime() || oplog.getPostImageOpTime());
    auto opTime = oplog.getPreImageOpTime() ? oplog.getPreImageOpTime().value()
                                            : oplog.getPostImageOpTime().value();

    DBDirectClient client(opCtx);
    auto oplogDoc = client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                                   opTime.asQuery(),
                                   nullptr,
                                   QueryOption_OplogReplay);

    uassert(40613,
            str::stream() << "oplog no longer contains the complete write history of this "
                             "transaction, log with opTime "
                          << opTime.toString()
                          << " cannot be found",
            !oplogDoc.isEmpty());

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogDoc));
    return oplogEntry.getObject().getOwned();
}

/**
 * Extracts the findAndModify result by inspecting the oplog entries that where generated by a
 * previous execution of the command. In the case of nested oplog entry where the correct links
 * are in the top level oplog, oplogWithCorrectLinks can be used to specify the outer oplog.
 */
void parseOplogEntryForFindAndModify(OperationContext* opCtx,
                                     const FindAndModifyRequest& request,
                                     const repl::OplogEntry& oplogEntry,
                                     const repl::OplogEntry& oplogWithCorrectLinks,
                                     BSONObjBuilder* builder) {
    validateFindAndModifyRetryability(request, oplogEntry, oplogWithCorrectLinks);

    switch (oplogEntry.getOpType()) {
        case repl::OpTypeEnum::kInsert:
            return find_and_modify::serializeUpsert(
                1,
                request.shouldReturnNew() ? oplogEntry.getObject() : boost::optional<BSONObj>(),
                false,
                oplogEntry.getObject(),
                builder);
        case repl::OpTypeEnum::kUpdate:
            return find_and_modify::serializeUpsert(
                1, extractPreOrPostImage(opCtx, oplogWithCorrectLinks), true, {}, builder);
        case repl::OpTypeEnum::kDelete:
            return find_and_modify::serializeRemove(
                1, extractPreOrPostImage(opCtx, oplogWithCorrectLinks), builder);
        default:
            MONGO_UNREACHABLE;
    }
}

repl::OplogEntry getInnerNestedOplogEntry(const repl::OplogEntry& entry) {
    uassert(40635,
            str::stream() << "expected nested oplog entry with ts: "
                          << entry.getTimestamp().toString()
                          << " to have o2 field: "
                          << redact(entry.toBSON()),
            entry.getObject2());
    return uassertStatusOK(repl::OplogEntry::parse(*entry.getObject2()));
}

}  // namespace

SingleWriteResult parseOplogEntryForUpdate(const repl::OplogEntry& entry) {
    SingleWriteResult res;
    // Upserts are stored as inserts.
    if (entry.getOpType() == repl::OpTypeEnum::kInsert) {
        res.setN(1);
        res.setNModified(0);

        BSONObjBuilder upserted;
        upserted.append(entry.getObject()["_id"]);
        res.setUpsertedId(upserted.obj());
    } else if (entry.getOpType() == repl::OpTypeEnum::kUpdate) {
        res.setN(1);
        res.setNModified(1);
    } else if (entry.getOpType() == repl::OpTypeEnum::kNoop) {
        return parseOplogEntryForUpdate(getInnerNestedOplogEntry(entry));
    } else {
        uasserted(40638,
                  str::stream() << "update retry request is not compatible with previous write in "
                                   "the transaction of type: "
                                << OpType_serializer(entry.getOpType())
                                << ", oplogTs: "
                                << entry.getTimestamp().toString()
                                << ", oplog: "
                                << redact(entry.toBSON()));
    }

    return res;
}

void parseOplogEntryForFindAndModify(OperationContext* opCtx,
                                     const FindAndModifyRequest& request,
                                     const repl::OplogEntry& oplogEntry,
                                     BSONObjBuilder* builder) {
    // Migrated op case.
    if (oplogEntry.getOpType() == repl::OpTypeEnum::kNoop) {
        parseOplogEntryForFindAndModify(
            opCtx, request, getInnerNestedOplogEntry(oplogEntry), oplogEntry, builder);
    } else {
        parseOplogEntryForFindAndModify(opCtx, request, oplogEntry, oplogEntry, builder);
    }
}

}  // namespace mongo
