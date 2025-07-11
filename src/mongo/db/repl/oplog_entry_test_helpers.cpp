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

#include "mongo/db/repl/oplog_entry_test_helpers.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/storage_parameters_gen.h"

#include <boost/optional.hpp>

namespace mongo {
namespace repl {
namespace {
void populateTwoPhaseIndexBuildOplogEntry(BSONObjBuilder& oplogEntryBuilder,
                                          const UUID& indexBuildUUID,
                                          const BSONObj& keyPattern,
                                          const std::string& indexName) {
    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");
    //"indexes" : [ { "v" : 2, "key" : { "a" : 1 }, "name" : "a_1" } ] }
    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    BSONObjBuilder indexDoc;
    indexDoc.append("v", 2);
    indexDoc.append("key", keyPattern);
    indexDoc.append("name", indexName);
    indexesArr.append(indexDoc.obj());
    indexesArr.done();
}

}  // namespace
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<BSONObj> object2,
                                OperationSessionInfo sessionInfo,
                                Date_t wallClockTime,
                                const std::vector<StmtId>& stmtIds,
                                boost::optional<UUID> uuid,
                                boost::optional<OpTime> prevOpTime) {
    return {repl::DurableOplogEntry(opTime,                           // optime
                                    opType,                           // opType
                                    nss,                              // namespace
                                    uuid,                             // uuid
                                    boost::none,                      // fromMigrate
                                    boost::none,                      // checkExistenceForDiffInsert
                                    boost::none,                      // versionContext
                                    repl::OplogEntry::kOplogVersion,  // version
                                    object,                           // o
                                    object2,                          // o2
                                    sessionInfo,                      // sessionInfo
                                    boost::none,                      // upsert
                                    wallClockTime,                    // wall clock time
                                    stmtIds,                          // statement ids
                                    prevOpTime,  // optime of previous write within same transaction
                                    boost::none,    // pre-image optime
                                    boost::none,    // post-image optime
                                    boost::none,    // ShardId of resharding recipient
                                    boost::none,    // _id
                                    boost::none)};  // needsRetryImage
}

OplogEntry makeCommandOplogEntry(OpTime opTime,
                                 const NamespaceString& nss,
                                 const BSONObj& object,
                                 boost::optional<BSONObj> object2,
                                 boost::optional<UUID> uuid) {
    return makeOplogEntry(opTime,
                          OpTypeEnum::kCommand,
                          nss.getCommandNS(),
                          object,
                          object2,
                          {} /* sessionInfo */,
                          Date_t() /* wallClockTime*/,
                          {} /* stmtIds */,
                          uuid);
}

OplogEntry makeCommandOplogEntryWithSessionInfoAndStmtIds(OpTime opTime,
                                                          const NamespaceString& nss,
                                                          const BSONObj& command,
                                                          LogicalSessionId lsid,
                                                          TxnNumber txnNum,
                                                          std::vector<StmtId> stmtIds,
                                                          boost::optional<OpTime> prevOpTime) {
    OperationSessionInfo info;
    info.setSessionId(lsid);
    info.setTxnNumber(txnNum);
    return makeOplogEntry(opTime,
                          OpTypeEnum::kCommand,
                          nss.getCommandNS(),
                          command,
                          boost::none /* o2 */,
                          info /* sessionInfo */,
                          Date_t::min() /* wallClockTime -- required but not checked */,
                          stmtIds,
                          boost::none /* uuid */,
                          prevOpTime);
}

OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert) {
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kInsert,  // op type
                          nss,                  // namespace
                          documentToInsert,     // o
                          boost::none,          // o2
                          {},                   // session info
                          Date_t::now());       // wall clock time
}

OplogEntry makeDeleteDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToDelete) {
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kDelete,  // op type
                          nss,                  // namespace
                          documentToDelete,     // o
                          boost::none,          // o2
                          {},                   // session info
                          Date_t::now());       // wall clock time
}

OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument) {
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kUpdate,  // op type
                          nss,                  // namespace
                          updatedDocument,      // o
                          documentToUpdate,     // o2
                          {},                   // session info
                          Date_t::now());       // wall clock time
}

OplogEntry makeCreateIndexOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern,
                                     const UUID& uuid,
                                     const BSONObj& options) {
    BSONObjBuilder spec;
    spec.append("v", 2);
    spec.append("key", keyPattern);
    spec.append("name", indexName);
    spec.appendElementsUnique(options);

    BSONObj indexInfo;
    if (feature_flags::gFeatureFlagReplicateLocalCatalogIdentifiers.isEnabledAndIgnoreFCVUnsafe()) {
        indexInfo = BSON("createIndexes" << nss.coll() << "spec" << spec.obj());
    } else {
        BSONObjBuilder builder;
        builder.append("createIndexes", nss.coll());
        builder.appendElements(spec.obj());
        indexInfo = builder.obj();
    }

    return makeOplogEntry(opTime,
                          OpTypeEnum::kCommand,
                          nss.getCommandNS(),
                          indexInfo,
                          boost::none /* object2 */,
                          {} /* sessionInfo */,
                          Date_t() /* wallClockTime*/,
                          {} /* stmtIds */,
                          uuid);
}

OplogEntry makeStartIndexBuildOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         const std::string& indexName,
                                         const BSONObj& keyPattern,
                                         const UUID& uuid,
                                         const UUID& indexBuildUUID) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("startIndexBuild", nss.coll());
    populateTwoPhaseIndexBuildOplogEntry(oplogEntryBuilder, indexBuildUUID, keyPattern, indexName);

    return makeCommandOplogEntry(
        opTime, nss, oplogEntryBuilder.obj(), boost::none /* object2 */, uuid);
}

OplogEntry makeCommitIndexBuildOplogEntry(OpTime opTime,
                                          const NamespaceString& nss,
                                          const std::string& indexName,
                                          const BSONObj& keyPattern,
                                          const UUID& uuid,
                                          const UUID& indexBuildUUID) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("commitIndexBuild", nss.coll());
    populateTwoPhaseIndexBuildOplogEntry(oplogEntryBuilder, indexBuildUUID, keyPattern, indexName);

    return makeCommandOplogEntry(
        opTime, nss, oplogEntryBuilder.obj(), boost::none /* object2 */, uuid);
}

OplogEntry makeInsertDocumentOplogEntryWithSessionInfo(OpTime opTime,
                                                       const NamespaceString& nss,
                                                       const BSONObj& documentToInsert,
                                                       OperationSessionInfo info) {
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kInsert,  // op type
                          nss,                  // namespace
                          documentToInsert,     // o
                          boost::none,          // o2
                          info,                 // session info
                          Date_t::now());       // wall clock time
}

OplogEntry makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
    OpTime opTime,
    const NamespaceString& nss,
    boost::optional<UUID> uuid,
    const BSONObj& documentToInsert,
    LogicalSessionId lsid,
    TxnNumber txnNum,
    const std::vector<StmtId>& stmtIds,
    boost::optional<OpTime> prevOpTime) {
    OperationSessionInfo info;
    info.setSessionId(lsid);
    info.setTxnNumber(txnNum);
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kInsert,  // op type
                          nss,                  // namespace
                          documentToInsert,     // o
                          boost::none,          // o2
                          info,                 // session info
                          Date_t::now(),        // wall clock time
                          stmtIds,
                          uuid,
                          prevOpTime);  // previous optime in same session
}
}  // namespace repl
}  // namespace mongo
