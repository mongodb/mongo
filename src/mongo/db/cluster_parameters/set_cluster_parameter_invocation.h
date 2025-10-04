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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/write_ops/batched_command_response.h"

#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

class ServerParameterService {
public:
    virtual ServerParameter* get(StringData parameterName) = 0;
    virtual ~ServerParameterService() = default;
};

class ClusterParameterService final : public ServerParameterService {
public:
    ServerParameter* get(StringData parameterName) override;
};

class DBClientService {
public:
    virtual BatchedCommandResponse updateParameterOnDisk(
        BSONObj query,
        BSONObj update,
        const WriteConcernOptions&,
        const boost::optional<auth::ValidatedTenancyScope>&) = 0;
    virtual Timestamp getUpdateClusterTime(OperationContext*) = 0;
    virtual ~DBClientService() = default;
};

class ClusterParameterDBClientService final : public DBClientService {
public:
    ClusterParameterDBClientService(DBDirectClient& dbDirectClient) : _dbClient(dbDirectClient) {}
    BatchedCommandResponse updateParameterOnDisk(
        BSONObj query,
        BSONObj update,
        const WriteConcernOptions&,
        const boost::optional<auth::ValidatedTenancyScope>&) override;
    Timestamp getUpdateClusterTime(OperationContext*) override;

private:
    DBDirectClient& _dbClient;
};

class SetClusterParameterInvocation {
public:
    SetClusterParameterInvocation(std::unique_ptr<ServerParameterService> serverParameterService,
                                  DBClientService& dbClientService)
        : _sps(std::move(serverParameterService)), _dbService(dbClientService) {}

    bool invoke(OperationContext*,
                const SetClusterParameter&,
                boost::optional<Timestamp>,
                boost::optional<LogicalTime>,
                const WriteConcernOptions&,
                bool skipValidation = false);

    // Validate new parameter passed to setClusterParameter and generate the query and update fields
    // for the on-disk update.
    std::pair<BSONObj, BSONObj> normalizeParameter(OperationContext* opCtx,
                                                   BSONObj cmdParamObj,
                                                   boost::optional<Timestamp> clusterParameterTime,
                                                   boost::optional<LogicalTime> previousTime,
                                                   ServerParameter* sp,
                                                   const boost::optional<TenantId>& tenantId,
                                                   bool skipValidation);

private:
    std::unique_ptr<ServerParameterService> _sps;
    DBClientService& _dbService;
};
}  // namespace mongo
