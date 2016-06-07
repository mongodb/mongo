/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status_with.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;

class AddShardToZoneRequest {
public:
    /**
     * Parses the provided BSON content as the external addShardToZone command, and if it is
     * correct,
     * constructs an AddShardToZoneRequest object from it.
     *
     * {
     *   addShardToZone: <string shardName>,
     *   zone: <string zoneName>
     * }
     */
    static StatusWith<AddShardToZoneRequest> parseFromMongosCommand(const BSONObj& cmdObj);

    /**
     * Parses the provided BSON content as the internal _configsvrAddShardToZone command, and if it
     * contains the correct types, constructs an AddShardToZoneRequest object from it.
     *
     * {
     *   _configsvrAddShardToZone: <string shardName>,
     *   zone: <string zoneName>
     * }
     */
    static StatusWith<AddShardToZoneRequest> parseFromConfigCommand(const BSONObj& cmdObj);

    /**
     * Creates a serialized BSONObj of the internal _configsvrAddShardToZone command from this
     * AddShardToZoneRequest instance.
     */
    void appendAsConfigCommand(BSONObjBuilder* cmdBuilder);

    const std::string& getShardName() const;
    const std::string& getZoneName() const;

private:
    AddShardToZoneRequest(std::string shardName, std::string zoneName);
    static StatusWith<AddShardToZoneRequest> _parseFromCommand(const BSONObj& cmdObj,
                                                               bool forMongos);

    std::string _shardName;
    std::string _zoneName;
};

}  // namespace mongo
