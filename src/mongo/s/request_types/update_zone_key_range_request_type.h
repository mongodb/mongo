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
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

class BSONObjBuilder;

class UpdateZoneKeyRangeRequest {
public:
    /**
     * Parses the provided BSON content as the external updateZoneKeyRange command, and if it is
     * correct, constructs an UpdateZoneKeyRangeRequest object from it.
     *
     * {
     *   updateZoneKeyRange: <string namespace>,
     *   min: <BSONObj min>,
     *   max: <BSONObj max>,
     *   zone: <string zoneName>
     * }
     */
    static StatusWith<UpdateZoneKeyRangeRequest> parseFromMongosCommand(const BSONObj& cmdObj);

    /**
     * Parses the provided BSON content as the internal _configsvrUpdateZoneKeyRange command, and
     * if it contains the correct types, constructs an UpdateZoneKeyRangeRequest object from it.
     *
     * {
     *   _configsvrUpdateZoneKeyRange: <string namespace>,
     *   min: <BSONObj min>,
     *   max: <BSONObj max>,
     *   zone: <string zone|null>,
     *   writeConcern: <BSONObj>
     * }
     */
    static StatusWith<UpdateZoneKeyRangeRequest> parseFromConfigCommand(const BSONObj& cmdObj);

    /**
     * Creates a serialized BSONObj of the internal _configsvrRemoveShardFromZone command from this
     * UpdateZoneKeyRangeRequest instance.
     */
    void appendAsConfigCommand(BSONObjBuilder* cmdBuilder);

    const NamespaceString& getNS() const;
    const ChunkRange& getRange() const;

    /**
     * Note: This is invalid if isRemove is true.
     */
    const std::string& getZoneName() const;

    bool isRemove() const;

private:
    /**
     * Constructor for remove type UpdateZoneKeyRangeRequest.
     */
    UpdateZoneKeyRangeRequest(NamespaceString ns, ChunkRange range);

    /**
     * Constructor for assign type UpdateZoneKeyRangeRequest.
     */
    UpdateZoneKeyRangeRequest(NamespaceString ns, ChunkRange range, std::string zoneName);

    static StatusWith<UpdateZoneKeyRangeRequest> _parseFromCommand(const BSONObj& cmdObj,
                                                                   bool forMongos);
    NamespaceString _ns;
    ChunkRange _range;
    bool _isRemove;
    std::string _zoneName;
};

}  // namespace mongo
