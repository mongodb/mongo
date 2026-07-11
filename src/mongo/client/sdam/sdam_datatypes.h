// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <chrono>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>


/**
 * The data structures in this file are defined in the "Server Discovery & Monitoring"
 * specification, which governs how topology changes are detected in a cluster. See
 * https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst
 * for more information.
 */
namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] sdam {
enum class TopologyType {
    kSingle,
    kReplicaSetNoPrimary,
    kReplicaSetWithPrimary,
    kSharded,
    kUnknown
};
std::vector<TopologyType> allTopologyTypes();
std::string toString(TopologyType topologyType);
StatusWith<TopologyType> parseTopologyType(std::string_view strTopologyType);
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
StatusWith<ServerType> parseServerType(std::string_view strServerType);
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
            _topologyVersion = TopologyVersion::parse(topologyVersionField.Obj(),
                                                      IDLParserContext("TopologyVersion"));
        }
    }

    // Failure constructor.
    HelloOutcome(HostAndPort server, BSONObj response, std::string errorMsg)
        : _server(std::move(server)), _success(false), _errorMsg(errorMsg) {
        const auto topologyVersionField = response.getField("topologyVersion");
        if (topologyVersionField) {
            _topologyVersion = TopologyVersion::parse(topologyVersionField.Obj(),
                                                      IDLParserContext("TopologyVersion"));
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
}  // namespace sdam
}  // namespace mongo
