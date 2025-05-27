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

#include "mongo/db/operation_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/version_context.h"
#include "mongo/stdx/unordered_set.h"

#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace change_stream_serverless_helpers {

using TenantSet = stdx::unordered_set<TenantId, TenantId::Hasher>;

/**
 * Returns true if the server is configured such that change collections can be used to record
 * oplog entries; ie, we are running in a Serverless context. Returns false otherwise.
 */
bool isChangeCollectionsModeActive(const VersionContext& vCtx);

/**
 * Returns true if the change stream is enabled for the provided tenant, false otherwise.
 */
bool isChangeStreamEnabled(OperationContext* opCtx, const TenantId& tenantId);

/**
 * Returns true if services related to the serverless change stream can be initialized.
 * TODO SERVER-69960 Remove this function and use 'isChangeCollectionsModeActive' instead.
 */
bool canInitializeServices();

/**
 * Returns true if the target environment (replica-set or sharded-cluster) supports running change
 * stream in the serverless, false otherwise.
 */
bool isServerlessEnvironment();

/**
 * Returns an internal tenant id that will be used for testing purposes. This tenant id will not
 * conflict with any other tenant id.
 */
const TenantId& getTenantIdForTesting();

/**
 * If the provided 'tenantId' is missing and 'internalChangeStreamUseTenantIdForTesting' is true,
 * returns a special 'TenantId' for testing purposes. Otherwise, returns the provided 'tenantId'.
 */
boost::optional<TenantId> resolveTenantId(const VersionContext& vCtx,
                                          boost::optional<TenantId> tenantId);

/**
 * Returns the list of the tenants associated with a 'config' database.
 */
TenantSet getConfigDbTenants(OperationContext* opCtx);

/**
 * Returns the 'expireAfterSeconds' value from the 'changeStreams' cluster-wide parameter for the
 * given tenant.
 */
int64_t getExpireAfterSeconds(const TenantId& tenantId);

Date_t getCurrentTimeForChangeCollectionRemoval(OperationContext* opCtx);
}  // namespace change_stream_serverless_helpers
}  // namespace mongo
