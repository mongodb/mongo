/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/rpc/topology_version_gen.h"
#include <boost/optional.hpp>
#include <string>

namespace mongo {

class BSONObj;
class BSONObjBuilder;

/**
 * Response structure for the ismaster command.
 *
 * Only handles responses from mongos.
 */
class MongosIsMasterResponse {
public:
    static constexpr StringData kTopologyVersionFieldName = "topologyVersion"_sd;
    static constexpr StringData kIsMasterFieldName = "ismaster"_sd;
    static constexpr StringData kMsgFieldName = "msg"_sd;

    /**
     * Explicit constructor that sets the _topologyVersion field.
     */
    MongosIsMasterResponse(TopologyVersion topologyVersion);

    /**
     * Appends MongosIsMasterResponse fields to "builder".
     */
    void appendToBuilder(BSONObjBuilder* builder) const;

    TopologyVersion getTopologyVersion() const {
        return _topologyVersion;
    }

private:
    TopologyVersion _topologyVersion;
};

}  // namespace mongo
