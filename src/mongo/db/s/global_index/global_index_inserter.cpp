/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/global_index/global_index_inserter.h"

#include <fmt/format.h>

#include "mongo/db/s/global_index/global_index_entry_gen.h"
#include "mongo/db/transaction_api.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kGlobalIndex

using namespace fmt::literals;

namespace mongo {
namespace global_index {
namespace {
MONGO_FAIL_POINT_DEFINE(globalIndexInserterPauseAfterReadingSkipCollection);
}  // namespace

GlobalIndexInserter::GlobalIndexInserter(NamespaceString nss,
                                         StringData indexName,
                                         UUID indexUUID,
                                         std::shared_ptr<executor::TaskExecutor> executor)
    : _nss(std::move(nss)),
      _indexName(indexName.toString()),
      _indexUUID(std::move(indexUUID)),
      _executor(std::move(executor)) {}

NamespaceString GlobalIndexInserter::_skipIdNss() {
    return NamespaceString(NamespaceString::kConfigDb,
                           "{}.globalIndex.{}.skipList"_format(_nss.coll(), _indexName));
}

NamespaceString GlobalIndexInserter::_globalIndexNss() {
    return NamespaceString(_nss.db(), "{}.globalIndex.{}"_format(_nss.coll(), _indexName));
}

void GlobalIndexInserter::processDoc(OperationContext* opCtx,
                                     const BSONObj& indexKeyValues,
                                     const BSONObj& documentKey) {
    auto insertToGlobalIndexFn = [this,
                                  service = opCtx->getServiceContext(),
                                  indexKeyValues,
                                  documentKey](const txn_api::TransactionClient& txnClient,
                                               ExecutorPtr txnExec) {
        FindCommandRequest skipIdQuery(_skipIdNss());
        skipIdQuery.setFilter(BSON("_id" << documentKey));
        skipIdQuery.setLimit(1);

        return txnClient.exhaustiveFind(skipIdQuery)
            .thenRunOn(txnExec)
            .then([this, service, indexKeyValues, documentKey, &txnClient, txnExec](
                      const auto& skipIdDocResults) {
                auto client = service->makeClient("globalIndexInserter");
                auto opCtx = service->makeOperationContext(client.get());

                {
                    stdx::lock_guard<Client> lk(*client);
                    client->setSystemOperationKillableByStepdown(lk);
                }

                globalIndexInserterPauseAfterReadingSkipCollection.pauseWhileSet(opCtx.get());

                if (!skipIdDocResults.empty()) {
                    return SemiFuture<void>::makeReady();
                }

                write_ops::InsertCommandRequest globalIndexEntryInsert(_globalIndexNss());
                globalIndexEntryInsert.getWriteCommandRequestBase().setCollectionUUID(_indexUUID);
                GlobalIndexEntry indexEntry(indexKeyValues, documentKey);
                globalIndexEntryInsert.setDocuments({indexEntry.toBSON()});

                return txnClient.runCRUDOp({globalIndexEntryInsert}, {})
                    .thenRunOn(txnExec)
                    .then([this, documentKey, &txnClient](const auto& commandResponse) {
                        write_ops::InsertCommandRequest skipIdInsert(_skipIdNss());

                        skipIdInsert.setDocuments({BSON("_id" << documentKey)});
                        return txnClient.runCRUDOp({skipIdInsert}, {}).ignoreValue();
                    })
                    .semi();
            })
            .semi();
    };

    txn_api::SyncTransactionWithRetries txn(opCtx, _executor, nullptr);
    txn.run(opCtx, insertToGlobalIndexFn);
}

}  // namespace global_index
}  // namespace mongo
