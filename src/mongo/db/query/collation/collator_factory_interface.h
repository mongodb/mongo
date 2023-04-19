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

#include "mongo/db/query/collation/collator_interface.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

class BSONObj;
class ServiceContext;
template <typename T>
class StatusWith;

/**
 * An interface which can be used to retrieve a collator.
 */
class CollatorFactoryInterface {
    CollatorFactoryInterface(const CollatorFactoryInterface&) = delete;
    CollatorFactoryInterface& operator=(const CollatorFactoryInterface&) = delete;

public:
    CollatorFactoryInterface() = default;

    virtual ~CollatorFactoryInterface() {}

    /**
     * Returns the CollatorFactoryInterface object associated with the specified service context.
     * This method must only be called if a CollatorFactoryInterface has been set on the service
     * context.
     */
    static CollatorFactoryInterface* get(ServiceContext* serviceContext);

    /**
     * Sets the CollatorFactoryInterface object associated with the specified service context.
     */
    static void set(ServiceContext* serviceContext,
                    std::unique_ptr<CollatorFactoryInterface> collatorFactory);

    /**
     * Parses 'spec' and, on success, returns the corresponding CollatorInterface. If 'spec'
     * represents the simple collation, returns an OK status with a null pointer.
     *
     * Returns a non-OK status if 'spec' is invalid or otherwise cannot be converted into a
     * collator.
     *
     * Returns ErrorCodes::IncompatibleCollationVersion if the collator version does not match the
     * version requested in 'spec'.
     */
    virtual StatusWith<std::unique_ptr<CollatorInterface>> makeFromBSON(const BSONObj& spec) = 0;
};

/**
 * Returns a collator for the user-specified collation 'userCollation'.
 *
 * Note: The caller should check if 'userCollation' is not empty since the empty 'userCollation'
 * has the special meaning that the query follows the collection default collation that exists.
 */
inline std::unique_ptr<CollatorInterface> getUserCollator(OperationContext* opCtx,
                                                          const BSONObj& userCollation) {
    tassert(7542402, "Empty user collation", !userCollation.isEmpty());
    return uassertStatusOK(
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(userCollation));
}

/**
 * Resolves the collator to either the user-specified collation or, if none was specified, to
 * the collection-default collation and also returns a flag indicating whether the user-provided
 * collation matches the collection default collation.
 */
std::pair<std::unique_ptr<CollatorInterface>, ExpressionContext::CollationMatchesDefault>
resolveCollator(OperationContext* opCtx, BSONObj userCollation, const CollectionPtr& collection);

}  // namespace mongo
