// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class BSONObj;
class BSONObjBuilder;

class RemoveShardFromZoneRequest {
public:
    /**
     * Parses the provided BSON content as the external removeShardFromZone command, and if it is
     * correct, constructs a removeShardFromZoneRequest object from it.
     *
     * {
     *   removeShardFromZone: <string shardName>,
     *   zone: <string zoneName>
     * }
     */
    static StatusWith<RemoveShardFromZoneRequest> parseFromMongosCommand(const BSONObj& cmdObj);

    /**
     * Parses the provided BSON content as the internal _configsvrRemoveShardFromZone command, and
     * if it contains the correct types, constructs a removeShardFromZoneRequest object from it.
     *
     * {
     *   _configsvrRemoveShardFromZone: <string shardName>,
     *   zone: <string zoneName>
     * }
     */
    static StatusWith<RemoveShardFromZoneRequest> parseFromConfigCommand(const BSONObj& cmdObj);

    /**
     * Creates a serialized BSONObj of the internal _configsvrRemoveShardFromZone command from this
     * RemoveShardFromZoneRequest instance.
     */
    void appendAsConfigCommand(BSONObjBuilder* cmdBuilder);

    const std::string& getShardName() const;
    const std::string& getZoneName() const;

private:
    RemoveShardFromZoneRequest(std::string shardName, std::string zoneName);
    static StatusWith<RemoveShardFromZoneRequest> _parseFromCommand(const BSONObj& cmdObj,
                                                                    bool forMongos);

    std::string _shardName;
    std::string _zoneName;
};

}  // namespace mongo
