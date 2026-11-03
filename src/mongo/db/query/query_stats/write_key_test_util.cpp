// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/write_key_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/write_cmd_shape_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_stats {

std::unique_ptr<UpdateKey> makeUpdateKey(OperationContext* opCtx,
                                         std::string_view updateCmd,
                                         query_shape::CollectionType collectionType) {
    auto ucr = write_ops::UpdateCommandRequest::parseOwned(fromjson(updateCmd));
    ASSERT_EQ(ucr.getUpdates().size(), 1U);
    auto request = query_shape::makeUpdateRequest(ucr);
    return std::make_unique<UpdateKey>(
        ExpressionContextBuilder{}.fromRequest(opCtx, request).build(),
        ucr,
        request.getHint(),
        query_shape::makeUpdateShape(opCtx, request),
        collectionType);
}

std::unique_ptr<DeleteKey> makeDeleteKey(OperationContext* opCtx,
                                         std::string_view deleteCmd,
                                         query_shape::CollectionType collectionType) {
    auto dcr = write_ops::DeleteCommandRequest::parseOwned(fromjson(deleteCmd));
    ASSERT_EQ(dcr.getDeletes().size(), 1U);
    auto request = query_shape::makeDeleteRequest(dcr);
    return std::make_unique<DeleteKey>(
        ExpressionContextBuilder{}.fromRequest(opCtx, request).build(),
        dcr,
        request.getHint(),
        query_shape::makeDeleteShape(opCtx, request),
        collectionType);
}

std::unique_ptr<InsertKey> makeInsertKey(OperationContext* opCtx,
                                         std::string_view insertCmd,
                                         query_shape::CollectionType collectionType) {
    auto icr = write_ops::InsertCommandRequest::parseOwned(fromjson(insertCmd));
    return std::make_unique<InsertKey>(
        opCtx,
        icr,
        std::make_unique<query_shape::InsertCmdShape>(icr.getNamespace()),
        collectionType);
}

}  // namespace mongo::query_stats
