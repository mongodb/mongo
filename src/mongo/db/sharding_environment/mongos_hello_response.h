// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class BSONObj;
class BSONObjBuilder;

/**
 * Response structure for the hello command.
 *
 * Only handles responses from mongos.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] MongosHelloResponse {
public:
    static constexpr std::string_view kTopologyVersionFieldName = "topologyVersion"sv;
    static constexpr std::string_view kIsMasterFieldName = "ismaster"sv;
    static constexpr std::string_view kIsWritablePrimaryFieldName = "isWritablePrimary"sv;
    static constexpr std::string_view kMsgFieldName = "msg"sv;

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
