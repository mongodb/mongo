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


#include "mongo/bson/bsonobj.h"
#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_chunk_cloner_source.h"


#include "mongo/db/concurrency/locker.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_source_manager.h"

namespace mongo {

MigrationChunkClonerSource::MigrationChunkClonerSource() = default;

MigrationChunkClonerSource::~MigrationChunkClonerSource() = default;

LogInsertForShardingHandler::LogInsertForShardingHandler(OperationContext* opCtx,
                                                         NamespaceString nss,
                                                         BSONObj doc,
                                                         repl::OpTime opTime)
    : _opCtx(opCtx), _nss(std::move(nss)), _doc(doc.getOwned()), _opTime(std::move(opTime)) {}

void LogInsertForShardingHandler::commit(boost::optional<Timestamp>) {
    // TODO (SERVER-71444): Fix to be interruptible or document exception.
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());  // NOLINT.

    auto csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(_opCtx, csr);

    if (auto cloner = MigrationSourceManager::getCurrentCloner(csr, csrLock)) {
        cloner->onInsertOp(_opCtx, _doc, _opTime);
    }
}

LogUpdateForShardingHandler::LogUpdateForShardingHandler(OperationContext* opCtx,
                                                         NamespaceString nss,
                                                         boost::optional<BSONObj> preImageDoc,
                                                         BSONObj postImageDoc,
                                                         repl::OpTime opTime,
                                                         repl::OpTime prePostImageOpTime)
    : _opCtx(opCtx),
      _nss(std::move(nss)),
      _preImageDoc(preImageDoc ? preImageDoc->getOwned() : boost::optional<BSONObj>(boost::none)),
      _postImageDoc(postImageDoc.getOwned()),
      _opTime(std::move(opTime)),
      _prePostImageOpTime(std::move(prePostImageOpTime)) {}

void LogUpdateForShardingHandler::commit(boost::optional<Timestamp>) {
    // TODO (SERVER-71444): Fix to be interruptible or document exception.
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());  // NOLINT.

    auto csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(_opCtx, csr);

    if (auto cloner = MigrationSourceManager::getCurrentCloner(csr, csrLock)) {
        cloner->onUpdateOp(_opCtx, _preImageDoc, _postImageDoc, _opTime, _prePostImageOpTime);
    }
}

LogDeleteForShardingHandler::LogDeleteForShardingHandler(OperationContext* opCtx,
                                                         NamespaceString nss,
                                                         repl::DocumentKey documentKey,
                                                         repl::OpTime opTime,
                                                         repl::OpTime prePostImageOpTime)
    : _opCtx(opCtx),
      _nss(std::move(nss)),
      _documentKey(std::move(documentKey)),
      _opTime(std::move(opTime)),
      _prePostImageOpTime(std::move(prePostImageOpTime)) {}

void LogDeleteForShardingHandler::commit(boost::optional<Timestamp>) {
    // TODO (SERVER-71444): Fix to be interruptible or document exception.
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());  // NOLINT.

    auto csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(_opCtx, csr);

    if (auto cloner = MigrationSourceManager::getCurrentCloner(csr, csrLock)) {
        cloner->onDeleteOp(_opCtx, _documentKey, _opTime, _prePostImageOpTime);
    }
}

}  // namespace mongo
