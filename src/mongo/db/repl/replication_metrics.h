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
#include "mongo/platform/mutex.h"

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

    // Election metrics
    void incrementNumElectionsCalledForReason(StartElectionReasonEnum reason);
    void incrementNumElectionsSuccessfulForReason(StartElectionReasonEnum reason);
    void incrementNumStepDownsCausedByHigherTerm();
    void incrementNumCatchUps();
    void incrementNumCatchUpsConcludedForReason(
        ReplicationCoordinator::PrimaryCatchUpConclusionReason reason);

    long getNumStepUpCmdsCalled_forTesting();
    long getNumPriorityTakeoversCalled_forTesting();
    long getNumCatchUpTakeoversCalled_forTesting();
    long getNumElectionTimeoutsCalled_forTesting();
    long getNumFreezeTimeoutsCalled_forTesting();
    long getNumStepUpCmdsSuccessful_forTesting();
    long getNumPriorityTakeoversSuccessful_forTesting();
    long getNumCatchUpTakeoversSuccessful_forTesting();
    long getNumElectionTimeoutsSuccessful_forTesting();
    long getNumFreezeTimeoutsSuccessful_forTesting();
    long getNumStepDownsCausedByHigherTerm_forTesting();
    long getNumCatchUps_forTesting();
    long getNumCatchUpsSucceeded_forTesting();
    long getNumCatchUpsAlreadyCaughtUp_forTesting();
    long getNumCatchUpsSkipped_forTesting();
    long getNumCatchUpsTimedOut_forTesting();
    long getNumCatchUpsFailedWithError_forTesting();
    long getNumCatchUpsFailedWithNewTerm_forTesting();
    long getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting();

    // Election candidate metrics

    // All the election candidate metrics that should be set when a node calls an election are set
    // in this one function, so that the 'electionCandidateMetrics' section of replSetStatus shows a
    // consistent state.
    void setElectionCandidateMetrics(const StartElectionReasonEnum reason,
                                     const Date_t lastElectionDate,
                                     const long long electionTerm,
                                     const OpTime lastCommittedOpTime,
                                     const OpTime lastSeenOpTime,
                                     const int numVotesNeeded,
                                     const double priorityAtElection,
                                     const Milliseconds electionTimeoutMillis,
                                     const boost::optional<int> priorPrimary);
    void setTargetCatchupOpTime(OpTime opTime);
    void setNumCatchUpOps(long numCatchUpOps);
    void setCandidateNewTermStartDate(Date_t newTermStartDate);
    void setWMajorityWriteAvailabilityDate(Date_t wMajorityWriteAvailabilityDate);

    boost::optional<OpTime> getTargetCatchupOpTime_forTesting();

    BSONObj getElectionMetricsBSON();
    BSONObj getElectionCandidateMetricsBSON();
    void clearElectionCandidateMetrics();

    // Election participant metrics

    // All the election participant metrics that should be set when a node votes in an election are
    // set in this one function, so that the 'electionParticipantMetrics' section of replSetStatus
    // shows a consistent state.
    void setElectionParticipantMetrics(const bool votedForCandidate,
                                       const long long electionTerm,
                                       const Date_t lastVoteDate,
                                       const int electionCandidateMemberId,
                                       const std::string voteReason,
                                       const OpTime lastAppliedOpTime,
                                       const OpTime maxAppliedOpTimeInSet,
                                       const double priorityAtElection);

    BSONObj getElectionParticipantMetricsBSON();
    void setParticipantNewTermDates(Date_t newTermStartDate, Date_t newTermAppliedDate);
    void clearParticipantNewTermDates();


private:
    class ElectionMetricsSSS;

    void _updateAverageCatchUpOps(WithLock lk);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReplicationMetrics::_mutex");
    ElectionMetrics _electionMetrics;
    ElectionCandidateMetrics _electionCandidateMetrics;
    ElectionParticipantMetrics _electionParticipantMetrics;

    bool _nodeIsCandidateOrPrimary = false;
    bool _nodeHasVotedInElection = false;

    // This field is a double so that the division result in _updateAverageCatchUpOps will be a
    // double without any casting.
    double _totalNumCatchUpOps = 0.0;
};

}  // namespace repl
}  // namespace mongo
