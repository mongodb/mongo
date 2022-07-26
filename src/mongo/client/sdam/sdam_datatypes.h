/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <chrono>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"


/**
 * The data structures in this file are defined in the "Server Discovery & Monitoring"
 * specification, which governs how topology changes are detected in a cluster. See
 * https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst
 * for more information.
 */
namespace mongo::sdam {
enum class TopologyType {
    kSingle,
    kReplicaSetNoPrimary,
    kReplicaSetWithPrimary,
    kSharded,
    kUnknown
};
std::vector<TopologyType> allTopologyTypes();
std::string toString(TopologyType topologyType);
StatusWith<TopologyType> parseTopologyType(StringData strTopologyType);
std::ostream& operator<<(std::ostream& os, TopologyType topologyType);

enum class ServerType {
    kStandalone,
    kMongos,
    kRSPrimary,
    kRSSecondary,
    kRSArbiter,
    kRSOther,
    kRSGhost,
    kUnknown
};
std::vector<ServerType> allServerTypes();
std::string toString(ServerType serverType);
StatusWith<ServerType> parseServerType(StringData strServerType);
std::ostream& operator<<(std::ostream& os, ServerType serverType);

using HelloRTT = Microseconds;

// The result of an attempt to call the "hello" command on a server.
class HelloOutcome {
    HelloOutcome() = delete;

public:
    // Success constructor.
    HelloOutcome(HostAndPort server, BSONObj response, boost::optional<HelloRTT> rtt = boost::none)
        : _server(std::move(server)), _success(true), _response(response), _rtt(rtt) {
        const auto topologyVersionField = response.getField("topologyVersion");
        if (topologyVersionField) {
            _topologyVersion = TopologyVersion::parse(IDLParserContext("TopologyVersion"),
                                                      topologyVersionField.Obj());
        }
    }

    // Failure constructor.
    HelloOutcome(HostAndPort server, BSONObj response, std::string errorMsg)
        : _server(std::move(server)), _success(false), _errorMsg(errorMsg) {
        const auto topologyVersionField = response.getField("topologyVersion");
        if (topologyVersionField) {
            _topologyVersion = TopologyVersion::parse(IDLParserContext("TopologyVersion"),
                                                      topologyVersionField.Obj());
        }
    }

    const HostAndPort& getServer() const;
    bool isSuccess() const;
    const boost::optional<BSONObj>& getResponse() const;
    const boost::optional<HelloRTT>& getRtt() const;
    const boost::optional<TopologyVersion>& getTopologyVersion() const;
    const std::string& getErrorMsg() const;
    BSONObj toBSON() const;

private:
    HostAndPort _server;
    // Indicates the success or failure of the attempt.
    bool _success;
    // An error message in case of failure.
    std::string _errorMsg;
    // A document containing the command response (or boost::none if it failed).
    boost::optional<BSONObj> _response;
    // The round trip time to execute the command (or boost::none if it failed or is not the outcome
    // from an initial handshake exchange).
    boost::optional<HelloRTT> _rtt;
    // Indicates how fresh the topology information in this reponse is (or boost::none if it failed
    // or the response did not include this).
    boost::optional<TopologyVersion> _topologyVersion;
};

class ServerDescription;
using ServerDescriptionPtr = std::shared_ptr<ServerDescription>;

class TopologyDescription;
using TopologyDescriptionPtr = std::shared_ptr<TopologyDescription>;

class TopologyManager;
using TopologyManagerPtr = std::unique_ptr<TopologyManager>;

class TopologyListener;
using TopologyListenerPtr = std::weak_ptr<TopologyListener>;
};  // namespace mongo::sdam
