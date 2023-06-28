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

#include <boost/smart_ptr.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/s/global_index/global_index_util.h"
#include "mongo/db/s/global_index_crud_commands_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"

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
    return skipIdNss(_nss, _indexName);
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
                globalIndexInserterPauseAfterReadingSkipCollection.pauseWhileSet(opCtx.get());

                if (!skipIdDocResults.empty()) {
                    return SemiFuture<void>::makeReady();
                }

                InsertGlobalIndexKey globalIndexEntryInsert(_indexUUID);
                // Note: dbName is unused by command but required by idl.
                globalIndexEntryInsert.setDbName(DatabaseName::kAdmin);
                globalIndexEntryInsert.setGlobalIndexKeyEntry(
                    GlobalIndexKeyEntry(indexKeyValues, documentKey));

                return txnClient.runCommandChecked(_nss.dbName(), globalIndexEntryInsert.toBSON({}))
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

    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    txn_api::SyncTransactionWithRetries txn(opCtx, _executor, nullptr, inlineExecutor);
    txn.run(opCtx, insertToGlobalIndexFn);
}

}  // namespace global_index
}  // namespace mongo
