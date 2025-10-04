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

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status_with.h"

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
