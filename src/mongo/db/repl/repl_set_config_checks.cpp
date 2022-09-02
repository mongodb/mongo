/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_set_config_checks.h"

#include <algorithm>
#include <iterator>

#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

namespace {

/**
 * Checks that the priorities of all the arbiters in the configuration are 0.  If they were 1,
 * they should have been set to 0 in MemberConfig::initialize().  Otherwise, they are illegal.
 */
Status validateArbiterPriorities(const ReplSetConfig& config) {
    for (ReplSetConfig::MemberIterator iter = config.membersBegin(); iter != config.membersEnd();
         ++iter) {
        if (iter->isArbiter() && iter->getPriority() != 0) {
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          str::stream() << "Member " << iter->getHostAndPort().toString()
                                        << " is an arbiter but has priority " << iter->getPriority()
                                        << ". Arbiter priority must be 0.");
        }
    }
    return Status::OK();
}

/**
 * Checks that a configuration has no member with a "newlyAdded" field.  Such configurations are
 * valid for startup and reconfig but not for initiating a new replica set.
 */
Status ensureNoNewlyAddedMembers(const ReplSetConfig& config) {
    for (ReplSetConfig::MemberIterator iter = config.membersBegin(); iter != config.membersEnd();
         ++iter) {
        if (iter->isNewlyAdded()) {
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          str::stream()
                              << "Member " << iter->getHostAndPort().toString() << ", with "
                              << MemberConfig::kIdFieldName << " " << iter->getId()
                              << ", has a newlyAdded field, which is not valid for "
                                 "initial configuration of a replica set.");
        }
    }
    return Status::OK();
}

/**
 * Compares two initialized and validated replica set configurations and checks to see if the
 * transition from 'oldConfig' to 'newConfig' adds or removes at most 1 voting node.
 *
 * Assumes that the member id uniquely identifies a logical replica set node. We use the set of
 * member ids in the old and new config to determine the safety of the single node change.
 */
Status validateSingleNodeChange(const ReplSetConfig& oldConfig, const ReplSetConfig& newConfig) {
    // Add MemberId of voting nodes from each config into respective sets.
    std::set<MemberId> oldIdSet, newIdSet;
    for (MemberConfig m : oldConfig.votingMembers()) {
        oldIdSet.insert(m.getId());
    }
    for (MemberConfig m : newConfig.votingMembers()) {
        newIdSet.insert(m.getId());
    }

    //
    // The symmetric difference between the id sets of each config is the set of ids that are
    // present in either set but not in their intersection. A set X can be transformed into set Y
    // with 1 addition or removal operation if and only if their symmetric difference is equal to 1.
    // If the symmetric difference is 1, there are two possibilities:
    //
    // (1) There is exactly 1 element e in Y that does not appear in X. In this case we can
    // transform X to Y by adding the element e to X.
    //
    // (2) There is exactly 1 element in X that does not appear in Y. In this case we can transform
    // X to Y by removing the element e from X.
    //

    // The symmetric difference can't be larger than the union of both sets.
    std::vector<MemberId> symmDiff(oldIdSet.size() + newIdSet.size());
    auto diffEndIt = std::set_symmetric_difference(
        oldIdSet.begin(), oldIdSet.end(), newIdSet.begin(), newIdSet.end(), symmDiff.begin());
    auto symmDiffSize = std::distance(symmDiff.begin(), diffEndIt);

    if (symmDiffSize > 1) {
        return Status(ErrorCodes::InvalidReplicaSetConfig,
                      str::stream() << "Non force replica set reconfig can only add or remove at "
                                       "most 1 voting member.");
    }
    return Status::OK();
}

/**
 * Compares two initialized and validated replica set configurations, and checks to
 * see if "newConfig" is a legal successor configuration to "oldConfig".
 *
 * Returns Status::OK() if "newConfig" may replace "oldConfig", or an indicative error
 * otherwise.
 *
 * The checks performed by this test are necessary, but may not be sufficient for
 * ensuring that "newConfig" is a legal successor to "oldConfig".  For example,
 * a legal reconfiguration must typically be executed on a node that is currently
 * primary under "oldConfig" and is electable under "newConfig".  Such checks that
 * require knowledge of which node is executing the configuration are out of scope
 * for this function.
 *
 * When "force" is true, skips config version check, since the version is guaranteed
 * to be valid either by "force" reconfig command or by internal use.
 */
Status validateOldAndNewConfigsCompatible(const ReplSetConfig& oldConfig,
                                          const ReplSetConfig& newConfig) {
    invariant(newConfig.isInitialized());
    invariant(oldConfig.isInitialized());

    if (oldConfig.getConfigVersionAndTerm() >= newConfig.getConfigVersionAndTerm()) {
        return Status(
            ErrorCodes::NewReplicaSetConfigurationIncompatible,
            str::stream()
                << "New replica set configuration version and term must be greater than old, but "
                << newConfig.getConfigVersionAndTerm().toString() << " is not greater than "
                << oldConfig.getConfigVersionAndTerm().toString() << " for replica set "
                << newConfig.getReplSetName());
    }

    if (oldConfig.getReplSetName() != newConfig.getReplSetName()) {
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      str::stream() << "New and old configurations differ in replica set name; "
                                       "old was "
                                    << oldConfig.getReplSetName() << ", and new is "
                                    << newConfig.getReplSetName());
    }

    if (oldConfig.getReplicaSetId() != newConfig.getReplicaSetId()) {
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      str::stream() << "New and old configurations differ in replica set ID; "
                                       "old was "
                                    << oldConfig.getReplicaSetId() << ", and new is "
                                    << newConfig.getReplicaSetId());
    }

    if (oldConfig.getConfigServer() && !newConfig.getConfigServer()) {
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      str::stream() << "Cannot remove \"" << ReplSetConfig::kConfigServerFieldName
                                    << "\" from replica set configuration on reconfig");
    }

    //
    // For every member config mNew in newConfig, if there exists member config mOld
    // in oldConfig such that mNew.getHostAndPort() == mOld.getHostAndPort(), it is required
    // that mNew.getId() == mOld.getId().
    //
    // Also, one may not use reconfig to change the value of the buildIndexes or
    // arbiterOnly flags.
    //
    for (ReplSetConfig::MemberIterator mNew = newConfig.membersBegin();
         mNew != newConfig.membersEnd();
         ++mNew) {
        for (ReplSetConfig::MemberIterator mOld = oldConfig.membersBegin();
             mOld != oldConfig.membersEnd();
             ++mOld) {
            const bool idsEqual = mOld->getId() == mNew->getId();
            const bool hostsEqual = mOld->getHostAndPort() == mNew->getHostAndPort();
            if (!idsEqual && !hostsEqual) {
                continue;
            }
            if (hostsEqual && !idsEqual) {
                return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                              str::stream()
                                  << "New and old configurations both have members with "
                                  << MemberConfig::kHostFieldName << " of "
                                  << mOld->getHostAndPort().toString()
                                  << " but in the new configuration the "
                                  << MemberConfig::kIdFieldName << " field is " << mNew->getId()
                                  << " and in the old configuration it is " << mOld->getId()
                                  << " for replica set " << newConfig.getReplSetName());
            }
            // At this point, the _id and host fields are equal, so we're looking at the old and
            // new configurations for the same member node.
            const bool buildIndexesFlagsEqual =
                mOld->shouldBuildIndexes() == mNew->shouldBuildIndexes();
            if (!buildIndexesFlagsEqual) {
                return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                              str::stream()
                                  << "New and old configurations differ in the setting of the "
                                     "buildIndexes field for member "
                                  << mOld->getHostAndPort().toString()
                                  << "; to make this change, remove then re-add the member");
            }
            const bool arbiterFlagsEqual = mOld->isArbiter() == mNew->isArbiter();
            if (!arbiterFlagsEqual) {
                return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                              str::stream()
                                  << "New and old configurations differ in the setting of the "
                                     "arbiterOnly field for member "
                                  << mOld->getHostAndPort().toString()
                                  << "; to make this change, remove then re-add the member");
            }
        }
    }

    if (!enableReconfigRollbackCommittedWritesCheck.load()) {
        // Skip the following check. This parameter can only be set to false in tests.
        return Status::OK();
    }

    const int numVotersOldConfig =
        std::count_if(oldConfig.membersBegin(),
                      oldConfig.membersEnd(),
                      // Use 'getBaseNumVotes()' since a node may be newly added at this point,
                      // which would indicate that it temporarily has 'votes: 0'.
                      [](const auto& x) { return x.getBaseNumVotes() > 0; });
    const int numArbitersOldConfig = std::count_if(oldConfig.membersBegin(),
                                                   oldConfig.membersEnd(),
                                                   [](const auto& x) { return x.isArbiter(); });
    const int majorityVoteCountOldConfig = numVotersOldConfig / 2 + 1;
    const int writableVotingMembersCountOldConfig = numVotersOldConfig - numArbitersOldConfig;

    // An overlap between an election and write quorum is guaranteed to exist if the number of
    // writable voting members is greater than or equal to the majority of voters. This is because
    // at least one writable voting member will be a part of the majority in any election. This
    // overlap is important so that if a candidate node that has not replicated recently committed
    // writes decides to run for election, the writable voting member participating in the election
    // will not vote for the candidate. As a result, the candidate cannot successfully become the
    // primary.
    const auto overlapBetweenElectionAndWriteQuorumOldConfig =
        majorityVoteCountOldConfig <= writableVotingMembersCountOldConfig;
    const auto numElectableNodesNewConfig = std::count_if(
        newConfig.membersBegin(),
        newConfig.membersEnd(),
        // Use 'getBasePriority()' since newly added nodes also temporarily have 'priority: 0'.
        [](const MemberConfig& mem) { return mem.getBasePriority() > 0.0; });

    // If the aforementioned overlap doesn't exist, and we have a PSA set where the secondary can
    // run for election, there is a risk that the secondary will not have replicated recent majority
    // committed writes, but will be elected primary with the help of the arbiter. To prevent this
    // from happening,, we fail the reconfig and refer the user to the appropriate next steps.
    if (!overlapBetweenElectionAndWriteQuorumOldConfig && newConfig.isPSASet() &&
        numElectableNodesNewConfig > 1) {
        return Status(
            ErrorCodes::NewReplicaSetConfigurationIncompatible,
            str::stream()
                << "Rejecting reconfig where the new config has a PSA topology and the secondary "
                << "is electable, but the old config contains only one writable node. Refer to "
                << "https://docs.mongodb.com/manual/reference/method/rs.reconfigForPSASet/"
                << " for next steps on reconfiguring a PSA set.");
    }

    return Status::OK();
}
}  // namespace

/**
 * Checks if the node with the given config index is electable, returning a useful
 * status message if not.
 */
Status checkElectable(const ReplSetConfig& newConfig, int configIndex) {
    const MemberConfig& myConfig = newConfig.getMemberAt(configIndex);
    if (!myConfig.isElectable()) {
        return Status(ErrorCodes::NodeNotElectable,
                      str::stream() << "This node, " << myConfig.getHostAndPort().toString()
                                    << ", with _id " << myConfig.getId()
                                    << " is not electable under the new configuration with "
                                    << newConfig.getConfigVersionAndTerm().toString()
                                    << " for replica set " << newConfig.getReplSetName());
    }
    return Status::OK();
}

bool sameConfigContents(const ReplSetConfig& oldConfig, const ReplSetConfig& newConfig) {
    auto oldBSON = oldConfig.toBSON();
    auto newBSON = newConfig.toBSON();

    // Compare the two config objects ignoring the 'version' and 'term' fields.
    BSONObj ignoredFields = BSON("version" << 1 << "term" << 1);
    auto oldBSONFiltered = oldBSON.filterFieldsUndotted(ignoredFields, false);
    auto newBSONFiltered = newBSON.filterFieldsUndotted(ignoredFields, false);

    return oldBSONFiltered.woCompare(newBSONFiltered) == 0;
}

StatusWith<int> findSelfInConfig(ReplicationCoordinatorExternalState* externalState,
                                 const ReplSetConfig& newConfig,
                                 ServiceContext* ctx) {
    std::vector<ReplSetConfig::MemberIterator> meConfigs;
    for (ReplSetConfig::MemberIterator iter = newConfig.membersBegin();
         iter != newConfig.membersEnd();
         ++iter) {
        if (externalState->isSelfFastPath(iter->getHostAndPort())) {
            meConfigs.push_back(iter);
        }
    }
    if (meConfigs.empty()) {
        // No self-hosts were found using the fastpath; check with the slow path.
        for (ReplSetConfig::MemberIterator iter = newConfig.membersBegin();
             iter != newConfig.membersEnd();
             ++iter) {
            if (externalState->isSelfSlowPath(iter->getHostAndPort(), ctx, Seconds(30))) {
                meConfigs.push_back(iter);
            }
        }
    }
    if (meConfigs.empty()) {
        return StatusWith<int>(ErrorCodes::NodeNotFound,
                               str::stream() << "No host described in new configuration with "
                                             << newConfig.getConfigVersionAndTerm().toString()
                                             << " for replica set " << newConfig.getReplSetName()
                                             << " maps to this node");
    }
    if (meConfigs.size() > 1) {
        str::stream message;
        message << "The hosts " << meConfigs.front()->getHostAndPort().toString();
        for (size_t i = 1; i < meConfigs.size() - 1; ++i) {
            message << ", " << meConfigs[i]->getHostAndPort().toString();
        }
        message << " and " << meConfigs.back()->getHostAndPort().toString()
                << " all map to this node in new configuration with "
                << newConfig.getConfigVersionAndTerm().toString() << " for replica set "
                << newConfig.getReplSetName();
        return StatusWith<int>(ErrorCodes::InvalidReplicaSetConfig, message);
    }

    int myIndex = std::distance(newConfig.membersBegin(), meConfigs.front());
    invariant(myIndex >= 0 && myIndex < newConfig.getNumMembers());
    return StatusWith<int>(myIndex);
}

StatusWith<int> findSelfInConfigIfElectable(ReplicationCoordinatorExternalState* externalState,
                                            const ReplSetConfig& newConfig,
                                            ServiceContext* ctx) {
    StatusWith<int> result = findSelfInConfig(externalState, newConfig, ctx);
    if (result.isOK()) {
        Status status = checkElectable(newConfig, result.getValue());
        if (!status.isOK()) {
            return StatusWith<int>(status);
        }
    }
    return result;
}

int findOwnHostInConfigQuick(const ReplSetConfig& newConfig, HostAndPort host) {
    if (host.empty()) {
        return -1;
    }

    int firstMatchIndex = -1;
    int currIndex = 0;

    for (ReplSetConfig::MemberIterator iter = newConfig.membersBegin();
         iter != newConfig.membersEnd();
         ++iter) {

        if (iter->getHostAndPort() == host) {
            firstMatchIndex = currIndex;
            invariant(firstMatchIndex >= 0);
            break;
        }

        currIndex++;
    }

    return firstMatchIndex;
}

StatusWith<int> validateConfigForStartUp(ReplicationCoordinatorExternalState* externalState,
                                         const ReplSetConfig& newConfig,
                                         ServiceContext* ctx) {
    Status status = newConfig.validateAllowingSplitHorizonIP();
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }
    if (newConfig.containsCustomizedGetLastErrorDefaults()) {
        fassertFailedWithStatusNoTrace(
            5624100,
            {ErrorCodes::IllegalOperation,
             str::stream() << "Failed to start up: Replica set config contains customized "
                              "getLastErrorDefaults, which "
                              "has been deprecated and is now ignored. Use setDefaultRWConcern "
                              "instead to set a cluster-wide default writeConcern."});
    }
    return findSelfInConfig(externalState, newConfig, ctx);
}

StatusWith<int> validateConfigForInitiate(ReplicationCoordinatorExternalState* externalState,
                                          const ReplSetConfig& newConfig,
                                          ServiceContext* ctx) {
    Status status = newConfig.validate();
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }

    if (newConfig.containsCustomizedGetLastErrorDefaults()) {
        fassertFailedWithStatusNoTrace(
            5624101,
            {ErrorCodes::IllegalOperation,
             str::stream() << "Failed to initiate: Replica set config contains customized "
                              "getLastErrorDefaults, which "
                              "has been deprecated and is now ignored. Use setDefaultRWConcern "
                              "instead to set a cluster-wide default writeConcern."});
    }

    status = validateArbiterPriorities(newConfig);
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }

    if (!(status = ensureNoNewlyAddedMembers(newConfig)).isOK()) {
        return status;
    }

    if (newConfig.getConfigVersion() != 1) {
        return StatusWith<int>(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                               str::stream() << "Configuration used to initiate a replica set must "
                                             << " have version 1, but found "
                                             << newConfig.getConfigVersion());
    }

    if (newConfig.getConfigTerm() != OpTime::kInitialTerm) {
        return StatusWith<int>(
            ErrorCodes::NewReplicaSetConfigurationIncompatible,
            str::stream() << "Configuration used to initiate a replica set must have term "
                          << OpTime::kInitialTerm << ", but found " << newConfig.getConfigTerm());
    }
    return findSelfInConfigIfElectable(externalState, newConfig, ctx);
}

Status validateConfigForReconfig(const ReplSetConfig& oldConfig,
                                 const ReplSetConfig& newConfig,
                                 bool force,
                                 bool allowSplitHorizonIP) {
    Status status =
        allowSplitHorizonIP ? newConfig.validateAllowingSplitHorizonIP() : newConfig.validate();
    if (!status.isOK()) {
        return status;
    }

    uassert(5624102,
            "Failed to reconfig: Replica set config contains customized "
            "getLastErrorDefaults, which has "
            "been deprecated and is now ignored. Use setDefaultRWConcern instead to "
            "set a cluster-wide default writeConcern.",
            !newConfig.containsCustomizedGetLastErrorDefaults());

    status = validateOldAndNewConfigsCompatible(oldConfig, newConfig);
    if (!status.isOK()) {
        return status;
    }

    // For non-force reconfigs, verify that the reconfig only adds or removes a single node.
    // This ensures that all quorums of the new config overlap with all quorums of the old
    // config.
    if (!force) {
        status = validateSingleNodeChange(oldConfig, newConfig);
        if (!status.isOK()) {
            return status;
        }
    }

    status = validateArbiterPriorities(newConfig);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

StatusWith<int> validateConfigForHeartbeatReconfig(
    ReplicationCoordinatorExternalState* externalState,
    const ReplSetConfig& newConfig,
    HostAndPort ownHost,
    ServiceContext* ctx) {
    Status status = newConfig.validateAllowingSplitHorizonIP();
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }

    tassert(5624103,
            "Replica set config during heartbeat reconfig contains "
            "customized getLastErrorDefaults, which has "
            "been deprecated and is now ignored. Use setDefaultRWConcern instead to "
            "set a cluster-wide default writeConcern.",
            !newConfig.containsCustomizedGetLastErrorDefaults());

    auto quickIndex = findOwnHostInConfigQuick(newConfig, ownHost);
    if (quickIndex >= 0) {
        LOGV2(6475001,
              "Was able to quickly find new index in config. Skipping full isSelf checks",
              "index"_attr = quickIndex);
        return quickIndex;
    }

    return findSelfInConfig(externalState, newConfig, ctx);
}

}  // namespace repl
}  // namespace mongo
