// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/replica_set_monitor_server_parameters.h"
#include "mongo/client/replica_set_monitor_server_parameters_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {

/**
 * Test wrapper for tests that need to set and unset the replicaSetMonitorProtocol server parameter.
 */
class ReplicaSetMonitorProtocolTestUtil {
public:
    /**
     * Sets the replicaSetMonitorProtocol to 'protocol'.
     */
    static void setRSMProtocol(ReplicaSetMonitorProtocol protocol);

    /**
     * Restores the replicaSetMonitorProtocol parameter to its default value.
     */
    static void resetRSMProtocol();

private:
    /**
     * Finds the replicaSetMonitorProtocol ServerParameter.
     */
    static ServerParameter::Map::const_iterator findRSMProtocolServerParameter();

    static inline const std::string kRSMProtocolFieldName = "replicaSetMonitorProtocol";

    /**
     * A BSONObj containing the default for the replicaSetMonitorProtocol server parameter.
     */
    static inline const BSONObj kDefaultParameter =
        BSON(kRSMProtocolFieldName << toString(gReplicaSetMonitorProtocol));
};

}  // namespace mongo
