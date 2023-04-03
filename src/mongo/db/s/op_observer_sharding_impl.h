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

#include "mongo/db/op_observer/op_observer_impl.h"

namespace mongo {

class OpObserverShardingImpl : public OpObserverImpl {
public:
    OpObserverShardingImpl(std::unique_ptr<OplogWriter> oplogWriter);

    // True if the document being deleted belongs to a chunk which, while still in the shard,
    // is being migrated out. (Not to be confused with "fromMigrate", which tags operations
    // that are steps in performing the migration.)
    static bool isMigrating(OperationContext* opCtx,
                            NamespaceString const& nss,
                            BSONObj const& docToDelete);

protected:
    void shardObserveAboutToDelete(OperationContext* opCtx,
                                   NamespaceString const& nss,
                                   BSONObj const& docToDelete) override;
    void shardObserveInsertsOp(OperationContext* opCtx,
                               const NamespaceString& nss,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               const std::vector<repl::OpTime>& opTimeList,
                               const ShardingWriteRouter& shardingWriteRouter,
                               bool fromMigrate,
                               bool inMultiDocumentTransaction) override;
    void shardObserveUpdateOp(OperationContext* opCtx,
                              const NamespaceString& nss,
                              boost::optional<BSONObj> preImageDoc,
                              const BSONObj& updatedDoc,
                              const repl::OpTime& opTime,
                              const ShardingWriteRouter& shardingWriteRouter,
                              const repl::OpTime& prePostImageOpTime,
                              bool inMultiDocumentTransaction) override;
    void shardObserveDeleteOp(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& documentKey,
                              const repl::OpTime& opTime,
                              const ShardingWriteRouter& shardingWriteRouter,
                              const repl::OpTime& preImageOpTime,
                              bool inMultiDocumentTransaction) override;
    void shardObserveTransactionPrepareOrUnpreparedCommit(
        OperationContext* opCtx,
        const std::vector<repl::ReplOperation>& stmts,
        const repl::OpTime& prepareOrCommitOptime) override;
    void shardObserveNonPrimaryTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<repl::OplogEntry>& stmts,
        const repl::OpTime& prepareOrCommitOptime) override;
};

}  // namespace mongo
