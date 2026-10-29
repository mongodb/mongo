// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_metrics_gen.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PARENT_PRIVATE]] mongo {
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
    void setElectionCandidateMetrics(StartElectionReasonEnum reason,
                                     Date_t lastElectionDate,
                                     long long electionTerm,
                                     OpTime lastCommittedOpTime,
                                     OpTime latestWrittenOpTime,
                                     OpTime latestAppliedOpTime,
                                     int numVotesNeeded,
                                     double priorityAtElection,
                                     Milliseconds electionTimeoutMillis,
                                     boost::optional<int> priorPrimary);
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
    void setElectionParticipantMetrics(bool votedForCandidate,
                                       long long electionTerm,
                                       Date_t lastVoteDate,
                                       int electionCandidateMemberId,
                                       std::string voteReason,
                                       OpTime lastWrittenOpTime,
                                       OpTime maxWrittenOpTimeInSet,
                                       OpTime lastAppliedOpTime,
                                       OpTime maxAppliedOpTimeInSet,
                                       double priorityAtElection);

    BSONObj getElectionParticipantMetricsBSON();
    void setParticipantNewTermDates(Date_t newTermStartDate, Date_t newTermAppliedDate);
    void clearParticipantNewTermDates();


    class [[MONGO_MOD_PRIVATE]] ElectionMetricsSSS;

private:
    void _updateAverageCatchUpOps(WithLock lk);

    mutable std::mutex _mutex;
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
