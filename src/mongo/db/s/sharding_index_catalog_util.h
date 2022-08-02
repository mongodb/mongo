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
#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/task_executor.h"

namespace mongo {

namespace sharding_index_catalog_util {

/**
 * Registers a new index in the catalog so it will be available for the shard and router role. In
 * order to execute this function the following preconditions must be met:
 * - It should be used from a DDLCoordinator
 * - firstExecution should indicate if this is the first time that this function is being called
 * from the coordinator (it should be false even if the second execution happened after a stepdown).
 * If firstExecution is false, then the executor MUST come from a DDLCoordinator (or a POS).
 * - osi must contain a valid session id and transaction number
 *
 * We have the following guarantees:
 * - During the execution of this function migrations for userCollectionNss will be stopped and all
 * started migrations will be cancelled.
 * - There won't be any writes for userCollectionNss during the index catalog modification.
 */
void registerIndexCatalogEntry(OperationContext* opCtx,
                               std::shared_ptr<executor::TaskExecutor> executor,
                               OperationSessionInfo& osi,
                               const NamespaceString& userCollectionNss,
                               const std::string& name,
                               const BSONObj& keyPattern,
                               const BSONObj& options,
                               const UUID& collectionUUID,
                               const boost::optional<UUID>& indexCollectionUUID,
                               bool firstExecution);

/**
 * De-register an index from the catalog so it will no longer be available for shard and router
 * role. The same preconditions and guarantees of registerIndexCatalogEntry apply for this function.
 */
void unregisterIndexCatalogEntry(OperationContext* opCtx,
                                 std::shared_ptr<executor::TaskExecutor> executor,
                                 OperationSessionInfo& osi,
                                 const NamespaceString& userCollectionNss,
                                 const std::string& name,
                                 const UUID& collectionUUID,
                                 bool firstExecution);
}  // namespace sharding_index_catalog_util

}  // namespace mongo
