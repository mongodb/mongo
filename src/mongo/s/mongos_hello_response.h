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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/topology_version_gen.h"

#include <string>

#include <boost/optional.hpp>

namespace mongo {

class BSONObj;
class BSONObjBuilder;

/**
 * Response structure for the hello command.
 *
 * Only handles responses from mongos.
 */
class MongosHelloResponse {
public:
    static constexpr StringData kTopologyVersionFieldName = "topologyVersion"_sd;
    static constexpr StringData kIsMasterFieldName = "ismaster"_sd;
    static constexpr StringData kIsWritablePrimaryFieldName = "isWritablePrimary"_sd;
    static constexpr StringData kMsgFieldName = "msg"_sd;

    /**
     * Explicit constructor that sets the _topologyVersion field.
     */
    MongosHelloResponse(TopologyVersion topologyVersion);

    /**
     * Appends MongosHelloResponse fields to "builder". When true, "useLegacyResponseFields"
     * indicates that we are responding to an isMaster command and not a hello command. Attach
     * the legacy "ismaster" field if true, and the "isWritablePrimary" field otherwise.
     */
    void appendToBuilder(BSONObjBuilder* builder, bool useLegacyResponseFields) const;

    TopologyVersion getTopologyVersion() const {
        return _topologyVersion;
    }

    bool getIsWritablePrimary() const {
        return _isWritablePrimary;
    }

    std::string getMsg() const {
        return _msg;
    }

private:
    TopologyVersion _topologyVersion;
    bool _isWritablePrimary;
    std::string _msg;
};

}  // namespace mongo
