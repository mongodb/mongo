// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

class ServerParameterService {
public:
    virtual ServerParameter* get(std::string_view parameterName) = 0;
    virtual ~ServerParameterService() = default;
};

class ClusterParameterService final : public ServerParameterService {
public:
    ServerParameter* get(std::string_view parameterName) override;
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
