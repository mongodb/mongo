// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/read_write_concern_defaults_cache_lookup_mongos.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

boost::optional<RWConcernDefault> readWriteConcernDefaultsCacheLookupMongoS(
    OperationContext* opCtx) {
    GetDefaultRWConcern configsvrRequest;
    configsvrRequest.setDbName(DatabaseName::kAdmin);

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto cmdResponse =
        uassertStatusOK(configShard->runCommand(opCtx,
                                                ReadPreferenceSetting(ReadPreference::Nearest),
                                                DatabaseName::kAdmin,
                                                configsvrRequest.toBSON(),
                                                Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(cmdResponse.commandStatus);

    return RWConcernDefault::parse(cmdResponse.response,
                                   IDLParserContext("readWriteConcernDefaultsCacheLookupMongoS"));
}

}  // namespace mongo
