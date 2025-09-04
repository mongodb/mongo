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
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/storage_parameters_gen.h"

#include <boost/optional.hpp>

namespace mongo {
namespace repl {

OplogEntry makeCommandOplogEntry(OpTime opTime,
                                 const NamespaceString& nss,
                                 const BSONObj& object,
                                 boost::optional<BSONObj> object2,
                                 boost::optional<UUID> uuid) {
    return {DurableOplogEntry{DurableOplogEntryParams{.opTime = opTime,
                                                      .opType = OpTypeEnum::kCommand,
                                                      .nss = nss.getCommandNS(),
                                                      .uuid = uuid,
                                                      .oField = object,
                                                      .o2Field = object2,
                                                      .wallClockTime = Date_t()}}};
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

    return {DurableOplogEntry{DurableOplogEntryParams{.opTime = opTime,
                                                      .opType = OpTypeEnum::kCommand,
                                                      .nss = nss.getCommandNS(),
                                                      .oField = command,
                                                      .sessionInfo = info,
                                                      .wallClockTime = Date_t::min(),
                                                      .statementIds = std::move(stmtIds),
                                                      .prevWriteOpTimeInTransaction = prevOpTime}}};
}

OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert) {
    return {DurableOplogEntry{DurableOplogEntryParams{.opTime = opTime,
                                                      .opType = OpTypeEnum::kInsert,
                                                      .nss = nss,
                                                      .oField = documentToInsert,
                                                      .wallClockTime = Date_t::now()}}};
}

OplogEntry makeDeleteDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToDelete) {
    return {DurableOplogEntry{DurableOplogEntryParams{.opTime = opTime,
                                                      .opType = OpTypeEnum::kDelete,
                                                      .nss = nss,
                                                      .oField = documentToDelete,
                                                      .wallClockTime = Date_t::now()}}};
}

OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument) {
    return {DurableOplogEntry{DurableOplogEntryParams{.opTime = opTime,
                                                      .opType = OpTypeEnum::kUpdate,
                                                      .nss = nss,
                                                      .oField = updatedDocument,
                                                      .o2Field = documentToUpdate,
                                                      .wallClockTime = Date_t::now()}}};
}

OplogEntry makeContainerInsertOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         StringData containerIdent,
                                         int64_t key,
                                         BSONBinData value) {
    return {DurableOplogEntry{DurableOplogEntryParams{
        .opTime = opTime,
        .opType = OpTypeEnum::kContainerInsert,
        .nss = nss,
        .container = containerIdent,
        .oField = BSON("k" << key << "v" << value),
        .wallClockTime = Date_t::now(),
    }}};
}

OplogEntry makeContainerInsertOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         StringData containerIdent,
                                         BSONBinData key,
                                         BSONBinData value) {
    return {DurableOplogEntry{DurableOplogEntryParams{
        .opTime = opTime,
        .opType = OpTypeEnum::kContainerInsert,
        .nss = nss,
        .container = containerIdent,
        .oField = BSON("k" << key << "v" << value),
        .wallClockTime = Date_t::now(),
    }}};
}

OplogEntry makeContainerDeleteOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         StringData containerIdent,
                                         int64_t key) {
    return {DurableOplogEntry{DurableOplogEntryParams{
        .opTime = opTime,
        .opType = OpTypeEnum::kContainerDelete,
        .nss = nss,
        .container = containerIdent,
        .oField = BSON("k" << key),
        .wallClockTime = Date_t::now(),
    }}};
}

OplogEntry makeContainerDeleteOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         StringData containerIdent,
                                         BSONBinData key) {
    return {DurableOplogEntry{DurableOplogEntryParams{
        .opTime = opTime,
        .opType = OpTypeEnum::kContainerDelete,
        .nss = nss,
        .container = containerIdent,
        .oField = BSON("k" << key),
        .wallClockTime = Date_t::now(),
    }}};
}

OplogEntry makeCreateIndexOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern,
                                     const UUID& uuid,
                                     const BSONObj& options,
                                     bool directoryPerDB,
                                     bool directoryForIndexes) {
    BSONObjBuilder spec;
    spec.append("v", 2);
    spec.append("key", keyPattern);
    spec.append("name", indexName);
    spec.appendElementsUnique(options);

    boost::optional<BSONObj> o2;
    if (feature_flags::gFeatureFlagReplicateLocalCatalogIdentifiers.isEnabledAndIgnoreFCVUnsafe()) {
        o2 = BSON("indexIdent" << ident::generateNewIndexIdent(
                                      nss.dbName(), directoryPerDB, directoryForIndexes)
                               << "directoryPerDB" << directoryPerDB << "directoryForIndexes"
                               << directoryForIndexes);
    }

    BSONObjBuilder oBuilder;
    oBuilder.append("createIndexes", nss.coll());
    oBuilder.appendElements(spec.obj());

    return {DurableOplogEntry{DurableOplogEntryParams{
        .opTime = opTime,
        .opType = OpTypeEnum::kCommand,
        .nss = nss.getCommandNS(),
        .uuid = uuid,
        .oField = oBuilder.obj(),
        .o2Field = o2,
        .wallClockTime = Date_t::now(),
    }}};
}

OplogEntry makeStartIndexBuildOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         const UUID& uuid,
                                         const UUID& indexBuildUUID,
                                         const IndexBuildInfo& indexBuildInfo) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("startIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");
    oplogEntryBuilder.append("indexes", BSON_ARRAY(indexBuildInfo.spec));

    return makeCommandOplogEntry(
        opTime,
        nss,
        oplogEntryBuilder.obj(),
        BSON("indexes" << BSON_ARRAY(BSON("indexIdent" << indexBuildInfo.indexIdent))
                       << "directoryPerDB" << indexBuildInfo.directoryPerDB << "directoryForIndexes"
                       << indexBuildInfo.directoryForIndexes),
        uuid);
}

OplogEntry makeCommitIndexBuildOplogEntry(OpTime opTime,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          const UUID& indexBuildUUID,
                                          const IndexBuildInfo& indexBuildInfo) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("commitIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");
    oplogEntryBuilder.append("indexes", BSON_ARRAY(indexBuildInfo.spec));

    return makeCommandOplogEntry(
        opTime, nss, oplogEntryBuilder.obj(), boost::none /* object2 */, uuid);
}

OplogEntry makeInsertDocumentOplogEntryWithSessionInfo(OpTime opTime,
                                                       const NamespaceString& nss,
                                                       const BSONObj& documentToInsert,
                                                       OperationSessionInfo info) {
    return {DurableOplogEntry{DurableOplogEntryParams{.opTime = opTime,
                                                      .opType = OpTypeEnum::kInsert,
                                                      .nss = nss,
                                                      .oField = documentToInsert,
                                                      .sessionInfo = info,
                                                      .wallClockTime = Date_t::now()}}};
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

    return {DurableOplogEntry{DurableOplogEntryParams{.opTime = opTime,
                                                      .opType = OpTypeEnum::kInsert,
                                                      .nss = nss,
                                                      .uuid = uuid,
                                                      .oField = documentToInsert,
                                                      .sessionInfo = info,
                                                      .wallClockTime = Date_t::now(),
                                                      .statementIds = stmtIds,
                                                      .prevWriteOpTimeInTransaction = prevOpTime}}};
}
}  // namespace repl
}  // namespace mongo
