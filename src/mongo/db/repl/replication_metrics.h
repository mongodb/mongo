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

#include "mongo/db/repl/replication_metrics_gen.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace repl {

/**
 * A service context decoration that stores metrics related to elections and replication health.
 */
class ReplicationMetrics {
public:
    static ReplicationMetrics& get(ServiceContext* svc);
    static ReplicationMetrics& get(OperationContext* opCtx);

    ReplicationMetrics();
    ~ReplicationMetrics();

    void incrementNumElectionsCalledForReason(TopologyCoordinator::StartElectionReason reason);
    void incrementNumElectionsSuccessfulForReason(TopologyCoordinator::StartElectionReason reason);
    void incrementNumStepDownsCausedByHigherTerm();
    void incrementNumCatchUps();
    void incrementNumCatchUpsConcludedForReason(
        ReplicationCoordinator::PrimaryCatchUpConclusionReason reason);

    int getNumStepUpCmdsCalled_forTesting();
    int getNumPriorityTakeoversCalled_forTesting();
    int getNumCatchUpTakeoversCalled_forTesting();
    int getNumElectionTimeoutsCalled_forTesting();
    int getNumFreezeTimeoutsCalled_forTesting();
    int getNumStepUpCmdsSuccessful_forTesting();
    int getNumPriorityTakeoversSuccessful_forTesting();
    int getNumCatchUpTakeoversSuccessful_forTesting();
    int getNumElectionTimeoutsSuccessful_forTesting();
    int getNumFreezeTimeoutsSuccessful_forTesting();
    int getNumStepDownsCausedByHigherTerm_forTesting();
    int getNumCatchUps_forTesting();
    int getNumCatchUpsSucceeded_forTesting();
    int getNumCatchUpsAlreadyCaughtUp_forTesting();
    int getNumCatchUpsSkipped_forTesting();
    int getNumCatchUpsTimedOut_forTesting();
    int getNumCatchUpsFailedWithError_forTesting();
    int getNumCatchUpsFailedWithNewTerm_forTesting();
    int getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting();

    BSONObj getElectionMetricsBSON();

private:
    class ElectionMetricsSSS;

    mutable stdx::mutex _mutex;
    ElectionMetrics _electionMetrics;
    ElectionCandidateMetrics _electionCandidateMetrics;
    ElectionParticipantMetrics _electionParticipantMetrics;
};

}  // namespace repl
}  // namespace mongo
