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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace MONGO_MOD_PUB mongo {

struct IndexBuildInfo;
namespace repl {
/**
 * Creates an OplogEntry with given parameters and preset defaults.
 */
OplogEntry makeOplogEntry(repl::OpTime opTime,
                          repl::OpTypeEnum opType,
                          NamespaceString nss,
                          BSONObj object,
                          boost::optional<BSONObj> object2 = boost::none,
                          OperationSessionInfo sessionInfo = {},
                          Date_t wallClockTime = Date_t(),
                          const std::vector<StmtId>& stmtIds = {},
                          boost::optional<UUID> uuid = boost::none,
                          boost::optional<OpTime> prevOpTime = boost::none);

/**
 * Creates an insert oplog entry with given optime and namespace.
 */
OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert);
/**
 * Creates a delete oplog entry with given optime and namespace.
 */
OplogEntry makeDeleteDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToDelete);
/**
 * Creates an update oplog entry with given optime and namespace.
 */
OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument);

OplogEntry makeContainerInsertOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         StringData containerIdent,
                                         int64_t key,
                                         BSONBinData value);

OplogEntry makeContainerInsertOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         StringData containerIdent,
                                         BSONBinData key,
                                         BSONBinData value);

OplogEntry makeContainerDeleteOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         StringData containerIdent,
                                         int64_t key);

OplogEntry makeContainerDeleteOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         StringData containerIdent,
                                         BSONBinData key);

/**
 * Creates an index creation entry with given optime and namespace.
 */
OplogEntry makeCreateIndexOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern,
                                     const UUID& uuid,
                                     const BSONObj& options = {});
/**
 * Creates a two-phase index build start oplog entry with a given optime, namespace, and index
 * build UUID.
 */
OplogEntry makeStartIndexBuildOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         const UUID& uuid,
                                         const UUID& indexBuildUUID,
                                         const IndexBuildInfo& indexBuildInfo,
                                         StringData indexIdent);

/**
 * Creates a two-phase index build commit oplog entry with a given optime, namespace, and index
 * build UUID.
 */
OplogEntry makeCommitIndexBuildOplogEntry(OpTime opTime,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          const UUID& indexBuildUUID,
                                          const IndexBuildInfo& indexBuildInfo);

/**
 * Creates an oplog entry for 'command' with the given 'optime', 'namespace' and optional 'uuid'.
 */
OplogEntry makeCommandOplogEntry(OpTime opTime,
                                 const NamespaceString& nss,
                                 const BSONObj& object,
                                 boost::optional<BSONObj> object2 = boost::none,
                                 boost::optional<UUID> uuid = boost::none);

/**
 * Creates an oplog entry for 'command' with the given 'optime', 'namespace' and session information
 */
OplogEntry makeCommandOplogEntryWithSessionInfoAndStmtIds(
    OpTime opTime,
    const NamespaceString& nss,
    const BSONObj& command,
    LogicalSessionId lsid,
    TxnNumber txnNum,
    std::vector<StmtId> stmtIds,
    boost::optional<OpTime> prevOpTime = boost::none);

/**
 * Creates an insert oplog entry with given optime, namespace and session info.
 */
OplogEntry makeInsertDocumentOplogEntryWithSessionInfo(OpTime opTime,
                                                       const NamespaceString& nss,
                                                       const BSONObj& documentToInsert,
                                                       OperationSessionInfo info);

OplogEntry makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
    OpTime opTime,
    const NamespaceString& nss,
    boost::optional<UUID> uuid,
    const BSONObj& documentToInsert,
    LogicalSessionId lsid,
    TxnNumber txnNum,
    const std::vector<StmtId>& stmtIds,
    boost::optional<OpTime> prevOpTime = boost::none);

BSONObj makeInsertApplyOpsEntry(const NamespaceString& nss, const UUID& uuid, const BSONObj& doc);
}  // namespace repl
}  // namespace MONGO_MOD_PUB mongo
