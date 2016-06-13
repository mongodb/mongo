/**
 *    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replica_set_config_checks.h"

#include <iterator>

#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/service_context.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {
/**
 * Finds the index of the one member configuration in "newConfig" that corresponds
 * to the current node (as identified by "externalState").
 *
 * Returns an error if the current node does not appear or appears multiple times in
 * "newConfig".
 */
StatusWith<int> findSelfInConfig(ReplicationCoordinatorExternalState* externalState,
                                 const ReplicaSetConfig& newConfig,
                                 ServiceContext* ctx) {
    std::vector<ReplicaSetConfig::MemberIterator> meConfigs;
    for (ReplicaSetConfig::MemberIterator iter = newConfig.membersBegin();
         iter != newConfig.membersEnd();
         ++iter) {
        if (externalState->isSelf(iter->getHostAndPort(), ctx)) {
            meConfigs.push_back(iter);
        }
    }
    if (meConfigs.empty()) {
        return StatusWith<int>(ErrorCodes::NodeNotFound,
                               str::stream() << "No host described in new configuration "
                                             << newConfig.getConfigVersion()
                                             << " for replica set "
                                             << newConfig.getReplSetName()
                                             << " maps to this node");
    }
    if (meConfigs.size() > 1) {
        str::stream message;
        message << "The hosts " << meConfigs.front()->getHostAndPort().toString();
        for (size_t i = 1; i < meConfigs.size() - 1; ++i) {
            message << ", " << meConfigs[i]->getHostAndPort().toString();
        }
        message << " and " << meConfigs.back()->getHostAndPort().toString()
                << " all map to this node in new configuration version "
                << newConfig.getConfigVersion() << " for replica set "
                << newConfig.getReplSetName();
        return StatusWith<int>(ErrorCodes::DuplicateKey, message);
    }

    int myIndex = std::distance(newConfig.membersBegin(), meConfigs.front());
    invariant(myIndex >= 0 && myIndex < newConfig.getNumMembers());
    return StatusWith<int>(myIndex);
}

/**
 * Checks if the node with the given config index is electable, returning a useful
 * status message if not.
 */
Status checkElectable(const ReplicaSetConfig& newConfig, int configIndex) {
    const MemberConfig& myConfig = newConfig.getMemberAt(configIndex);
    if (!myConfig.isElectable()) {
        return Status(ErrorCodes::NodeNotElectable,
                      str::stream() << "This node, " << myConfig.getHostAndPort().toString()
                                    << ", with _id "
                                    << myConfig.getId()
                                    << " is not electable under the new configuration version "
                                    << newConfig.getConfigVersion()
                                    << " for replica set "
                                    << newConfig.getReplSetName());
    }
    return Status::OK();
}

/**
 * Like findSelfInConfig, above, but also returns an error if the member configuration
 * for this node is not electable, as this is a requirement for nodes accepting
 * reconfig or initiate commands.
 */
StatusWith<int> findSelfInConfigIfElectable(ReplicationCoordinatorExternalState* externalState,
                                            const ReplicaSetConfig& newConfig,
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
 */
Status validateOldAndNewConfigsCompatible(const ReplicaSetConfig& oldConfig,
                                          const ReplicaSetConfig& newConfig) {
    invariant(newConfig.isInitialized());
    invariant(oldConfig.isInitialized());

    if (oldConfig.getConfigVersion() >= newConfig.getConfigVersion()) {
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      str::stream()
                          << "New replica set configuration version must be greater than old, but "
                          << newConfig.getConfigVersion()
                          << " is not greater than "
                          << oldConfig.getConfigVersion()
                          << " for replica set "
                          << newConfig.getReplSetName());
    }

    if (oldConfig.getReplSetName() != newConfig.getReplSetName()) {
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      str::stream() << "New and old configurations differ in replica set name; "
                                       "old was "
                                    << oldConfig.getReplSetName()
                                    << ", and new is "
                                    << newConfig.getReplSetName());
    }

    if (oldConfig.getReplicaSetId() != newConfig.getReplicaSetId()) {
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      str::stream() << "New and old configurations differ in replica set ID; "
                                       "old was "
                                    << oldConfig.getReplicaSetId()
                                    << ", and new is "
                                    << newConfig.getReplicaSetId());
    }

    if (oldConfig.isConfigServer() && !newConfig.isConfigServer()) {
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      str::stream() << "Cannot remove \""
                                    << ReplicaSetConfig::kConfigServerFieldName
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
    for (ReplicaSetConfig::MemberIterator mNew = newConfig.membersBegin();
         mNew != newConfig.membersEnd();
         ++mNew) {
        for (ReplicaSetConfig::MemberIterator mOld = oldConfig.membersBegin();
             mOld != oldConfig.membersEnd();
             ++mOld) {
            const bool idsEqual = mOld->getId() == mNew->getId();
            const bool hostsEqual = mOld->getHostAndPort() == mNew->getHostAndPort();
            if (!idsEqual && !hostsEqual) {
                continue;
            }
            if (hostsEqual && !idsEqual) {
                return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                              str::stream() << "New and old configurations both have members with "
                                            << MemberConfig::kHostFieldName
                                            << " of "
                                            << mOld->getHostAndPort().toString()
                                            << " but in the new configuration the "
                                            << MemberConfig::kIdFieldName
                                            << " field is "
                                            << mNew->getId()
                                            << " and in the old configuration it is "
                                            << mOld->getId()
                                            << " for replica set "
                                            << newConfig.getReplSetName());
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
    return Status::OK();
}
}  // namespace

StatusWith<int> validateConfigForStartUp(ReplicationCoordinatorExternalState* externalState,
                                         const ReplicaSetConfig& oldConfig,
                                         const ReplicaSetConfig& newConfig,
                                         ServiceContext* ctx) {
    Status status = newConfig.validate();
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }
    if (oldConfig.isInitialized()) {
        status = validateOldAndNewConfigsCompatible(oldConfig, newConfig);
        if (!status.isOK()) {
            return StatusWith<int>(status);
        }
    }
    return findSelfInConfig(externalState, newConfig, ctx);
}

StatusWith<int> validateConfigForInitiate(ReplicationCoordinatorExternalState* externalState,
                                          const ReplicaSetConfig& newConfig,
                                          ServiceContext* ctx) {
    Status status = newConfig.validate();
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }
    if (newConfig.getConfigVersion() != 1) {
        return StatusWith<int>(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                               str::stream() << "Configuration used to initiate a replica set must "
                                             << " have version 1, but found "
                                             << newConfig.getConfigVersion());
    }
    return findSelfInConfigIfElectable(externalState, newConfig, ctx);
}

StatusWith<int> validateConfigForReconfig(ReplicationCoordinatorExternalState* externalState,
                                          const ReplicaSetConfig& oldConfig,
                                          const ReplicaSetConfig& newConfig,
                                          ServiceContext* ctx,
                                          bool force) {
    Status status = newConfig.validate();
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }

    status = validateOldAndNewConfigsCompatible(oldConfig, newConfig);
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }

    if (force) {
        return findSelfInConfig(externalState, newConfig, ctx);
    }

    return findSelfInConfigIfElectable(externalState, newConfig, ctx);
}

StatusWith<int> validateConfigForHeartbeatReconfig(
    ReplicationCoordinatorExternalState* externalState,
    const ReplicaSetConfig& newConfig,
    ServiceContext* ctx) {
    Status status = newConfig.validate();
    if (!status.isOK()) {
        return StatusWith<int>(status);
    }

    return findSelfInConfig(externalState, newConfig, ctx);
}

}  // namespace repl
}  // namespace mongo
