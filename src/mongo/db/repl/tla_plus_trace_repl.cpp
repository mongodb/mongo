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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTlaPlusTrace

#include "mongo/platform/basic.h"

#include "mongo/db/repl/tla_plus_trace_repl.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/time_support.h"


namespace mongo {
namespace repl {

namespace {

constexpr StringData kSpecFieldName = "specs"_sd;

auto enabledForSpec(TLAPlusSpecEnum spec) {
    return [spec](const BSONObj& data) {
        auto array = data[kSpecFieldName].Array();
        return std::find_if(array.begin(), array.end(), [&spec](BSONElement elem) {
                   return elem.String() == TLAPlusSpec_serializer(spec);
               }) != array.end();
    };
}

/**
 *  Logs the provided message at a timestamp greater than the current time.
 */
void logEvent(std::string message) {
    Date_t beforeTime = Date_t::now();
    Date_t afterTime = Date_t::now();
    while (afterTime == beforeTime) {
        sleepmillis(1);
        afterTime = Date_t::now();
    }

    invariant(afterTime > beforeTime, "Clock went backwards");
    log() << message;
}

void logTlaPlusRaftMongoEvent(OperationContext* opCtx, RaftMongoSpecActionEnum action) {
    TlaPlusTraceEvent event;
    event.setSpec(TLAPlusSpecEnum::kRaftMongo);
    event.setAction(RaftMongoSpecAction_serializer(action));
    event.setHost(HostAndPort(getHostName(), serverGlobalParams.port));

    auto service = cc().getServiceContext();
    if (!service->getStorageEngine()) {
        LOG(2) << "Aborting logTlaPlusRaftMongoEvent: storage layer not yet initialized";
        return;
    }

    auto replCoord = ReplicationCoordinator::get(service);

    BSONObjBuilder bob;
    // Ignore other states not covered in the spec.
    bob.append("serverState",
               replCoord->getMemberState() == MemberState::RS_PRIMARY ? "Leader" : "Follower");
    bob.append("commitPoint", replCoord->getLastCommittedOpTime().toBSON());
    bob.append("term", replCoord->getTerm());
    event.setState(bob.obj());

    logEvent(event.toBSON().toString());
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(logForTLAPlusSpecs)

void tlaPlusRaftMongoEvent(OperationContext* opCtx, RaftMongoSpecActionEnum action) {
    if (MONGO_unlikely(
            logForTLAPlusSpecs.scopedIf(enabledForSpec(TLAPlusSpecEnum::kRaftMongo)).isActive())) {
        logTlaPlusRaftMongoEvent(opCtx, action);
    }
}
}  // namespace repl
}  // namespace mongo
