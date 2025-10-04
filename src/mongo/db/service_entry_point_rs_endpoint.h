/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/dbmessage.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/future.h"

namespace mongo {

/**
 * Replica-Set Endpoint specific service entry point.
 *
 * The Replica-Set Endpoint service is a "proxy service" that ordinarily will dispatch
 * requests to the router-role ServiceEntryPoint, but will dispatch particular requests
 * to the shard-role ServiceEntryPoint.
 */
class ServiceEntryPointRSEndpoint final : public ServiceEntryPoint {
public:
    ServiceEntryPointRSEndpoint(std::unique_ptr<ServiceEntryPointShardRole> shardSep)
        : _shardSep{std::move(shardSep)} {}

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request,
                                     Date_t started) final;

private:
    Future<DbResponse> _replicaSetEndpointHandleRequest(OperationContext* opCtx,
                                                        const Message& request,
                                                        Date_t started);

    std::unique_ptr<ServiceEntryPointShardRole> _shardSep;
};
}  // namespace mongo
