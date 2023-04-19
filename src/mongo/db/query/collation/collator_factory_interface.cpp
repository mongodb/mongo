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

#include "mongo/db/query/collation/collator_factory_interface.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

namespace {

const auto getCollatorFactory =
    ServiceContext::declareDecoration<std::unique_ptr<CollatorFactoryInterface>>();

}  // namespace

CollatorFactoryInterface* CollatorFactoryInterface::get(ServiceContext* serviceContext) {
    invariant(getCollatorFactory(serviceContext));
    return getCollatorFactory(serviceContext).get();
}

void CollatorFactoryInterface::set(ServiceContext* serviceContext,
                                   std::unique_ptr<CollatorFactoryInterface> collatorFactory) {
    getCollatorFactory(serviceContext) = std::move(collatorFactory);
}

std::pair<std::unique_ptr<CollatorInterface>, ExpressionContext::CollationMatchesDefault>
resolveCollator(OperationContext* opCtx, BSONObj userCollation, const CollectionPtr& collection) {
    if (!collection || !collection->getDefaultCollator()) {
        if (userCollation.isEmpty()) {
            return {nullptr, ExpressionContext::CollationMatchesDefault::kNoDefault};
        } else {
            return {getUserCollator(opCtx, userCollation),
                    ExpressionContext::CollationMatchesDefault::kNoDefault};
        }
    }

    auto defaultCollator = collection->getDefaultCollator()->clone();
    if (userCollation.isEmpty()) {
        return {std::move(defaultCollator), ExpressionContext::CollationMatchesDefault::kYes};
    }
    auto userCollator = getUserCollator(opCtx, userCollation);

    if (CollatorInterface::collatorsMatch(defaultCollator.get(), userCollator.get())) {
        return {std::move(defaultCollator), ExpressionContext::CollationMatchesDefault::kYes};
    } else {
        return {std::move(userCollator), ExpressionContext::CollationMatchesDefault::kNo};
    }
}

}  // namespace mongo
