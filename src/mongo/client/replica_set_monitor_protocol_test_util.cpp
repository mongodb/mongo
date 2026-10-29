// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/replica_set_monitor_protocol_test_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"

#include <map>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
void ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol protocol) {
    const BSONObj newParameterObj = BSON(kRSMProtocolFieldName << toString(protocol));
    BSONObjIterator parameterIterator(newParameterObj);
    BSONElement newParameter = parameterIterator.next();
    const auto foundParameter = findRSMProtocolServerParameter();

    uassertStatusOK(foundParameter->second->set(newParameter, boost::none));
}

void ReplicaSetMonitorProtocolTestUtil::resetRSMProtocol() {
    const auto defaultParameter = kDefaultParameter[kRSMProtocolFieldName];
    const auto foundParameter = findRSMProtocolServerParameter();

    uassertStatusOK(foundParameter->second->set(defaultParameter, boost::none));
}

ServerParameter::Map::const_iterator
ReplicaSetMonitorProtocolTestUtil::findRSMProtocolServerParameter() {
    const auto& parameterMap = ServerParameterSet::getNodeParameterSet()->getMap();
    invariant(parameterMap.size());
    return parameterMap.find(kRSMProtocolFieldName);
}

}  // namespace mongo
