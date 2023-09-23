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


#include <cstddef>
#include <memory>
#include <string>


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

using executor::RemoteCommandRequest;

QuorumChecker::QuorumChecker(const ReplSetConfig* rsConfig, int myIndex, long long term)
    : _rsConfig(rsConfig),
      _myIndex(myIndex),
      _term(term),
      _numResponses(1),  // We "responded" to ourself already.
      _numElectable(0),
      _vetoStatus(Status::OK()),
      _finalStatus(ErrorCodes::CallbackCanceled, "Quorum check canceled") {
    invariant(myIndex < _rsConfig->getNumMembers());
    const MemberConfig& myConfig = _rsConfig->getMemberAt(_myIndex);

    if (myConfig.isVoter()) {
        _voters.push_back(myConfig.getHostAndPort());
    }
    if (myConfig.isElectable()) {
        _numElectable = 1;
    }

    if (hasReceivedSufficientResponses()) {
        _onQuorumCheckComplete();
    }
}

QuorumChecker::~QuorumChecker() {}

std::vector<RemoteCommandRequest> QuorumChecker::getRequests() const {
    const bool isInitialConfig = _rsConfig->getConfigVersion() == 1;
    const MemberConfig& myConfig = _rsConfig->getMemberAt(_myIndex);

    std::vector<RemoteCommandRequest> requests;
    if (hasReceivedSufficientResponses()) {
        return requests;
    }

    BSONObj hbRequest;
    invariant(_term != OpTime::kUninitializedTerm);
    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setSetName(_rsConfig->getReplSetName());
    hbArgs.setConfigVersion(_rsConfig->getConfigVersion());
    hbArgs.setConfigTerm(_rsConfig->getConfigTerm());
    hbArgs.setHeartbeatVersion(1);
    if (isInitialConfig) {
        hbArgs.setCheckEmpty();
    }
    // hbArgs allows (but doesn't require) us to pass the current primary id as an optimization,
    // but it is not readily available within QuorumChecker.
    hbArgs.setSenderHost(myConfig.getHostAndPort());
    hbArgs.setSenderId(myConfig.getId().getData());
    hbArgs.setTerm(_term);
    hbRequest = hbArgs.toBSON();

    // Send a bunch of heartbeat requests.
    // Schedule an operation when a "sufficient" number of them have completed, and use that
    // to compute the quorum check results.
    // Wait for the "completion" callback to finish, and then it's OK to return the results.
    for (int i = 0; i < _rsConfig->getNumMembers(); ++i) {
        if (_myIndex == i) {
            // No need to check self for liveness or unreadiness.
            continue;
        }
        requests.push_back(RemoteCommandRequest(_rsConfig->getMemberAt(i).getHostAndPort(),
                                                DatabaseName::kAdmin,
                                                hbRequest,
                                                BSON(rpc::kReplSetMetadataFieldName << 1),
                                                nullptr,
                                                _rsConfig->getHeartbeatTimeoutPeriodMillis()));
    }

    return requests;
}

void QuorumChecker::processResponse(const RemoteCommandRequest& request,
                                    const executor::RemoteCommandResponse& response) {
    _tabulateHeartbeatResponse(request, response);
    if (hasReceivedSufficientResponses()) {
        _onQuorumCheckComplete();
    }
}

void QuorumChecker::_onQuorumCheckComplete() {
    if (!_vetoStatus.isOK()) {
        _finalStatus = _vetoStatus;
        return;
    }
    if (_rsConfig->getConfigVersion() == 1 && !_badResponses.empty()) {
        str::stream message;
        message << "replSetInitiate quorum check failed because not all proposed set members "
                   "responded affirmatively: ";
        for (std::vector<std::pair<HostAndPort, Status>>::const_iterator it = _badResponses.begin();
             it != _badResponses.end();
             ++it) {
            if (it != _badResponses.begin()) {
                message << ", ";
            }
            message << it->first.toString() << " failed with " << it->second.reason();
        }
        _finalStatus = Status(ErrorCodes::NodeNotFound, message);
        return;
    }
    if (_numElectable == 0) {
        _finalStatus = Status(ErrorCodes::NodeNotFound,
                              "Quorum check failed because no "
                              "electable nodes responded; at least one required for config");
        return;
    }
    if (int(_voters.size()) < _rsConfig->getMajorityVoteCount()) {
        str::stream message;
        message << "Quorum check failed because not enough voting nodes responded; required "
                << _rsConfig->getMajorityVoteCount() << " but ";

        if (_voters.size() == 0) {
            message << "none responded";
        } else {
            message << "only the following " << _voters.size()
                    << " voting nodes responded: " << _voters.front().toString();
            for (size_t i = 1; i < _voters.size(); ++i) {
                message << ", " << _voters[i].toString();
            }
        }
        if (!_badResponses.empty()) {
            message << "; the following nodes did not respond affirmatively: ";
            for (std::vector<std::pair<HostAndPort, Status>>::const_iterator it =
                     _badResponses.begin();
                 it != _badResponses.end();
                 ++it) {
                if (it != _badResponses.begin()) {
                    message << ", ";
                }
                message << it->first.toString() << " failed with " << it->second.reason();
            }
        }
        _finalStatus = Status(ErrorCodes::NodeNotFound, message);
        return;
    }
    _finalStatus = Status::OK();
}

void QuorumChecker::_tabulateHeartbeatResponse(const RemoteCommandRequest& request,
                                               const executor::RemoteCommandResponse& response) {
    ++_numResponses;
    if (!response.isOK()) {
        LOGV2_WARNING(23722,
                      "Failed to complete heartbeat request to {requestTarget}; {responseStatus}",
                      "Failed to complete heartbeat request to target",
                      "requestTarget"_attr = request.target,
                      "responseStatus"_attr = response.status);
        _badResponses.push_back(std::make_pair(request.target, response.status));
        return;
    }

    BSONObj resBSON = response.data;
    ReplSetHeartbeatResponse hbResp;
    Status hbStatus = hbResp.initialize(resBSON, 0);

    if (hbStatus.code() == ErrorCodes::InconsistentReplicaSetNames) {
        static constexpr char message[] = "Our set name did not match that of the request target";
        _vetoStatus =
            Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                   str::stream() << message << ", requestTarget:" << request.target.toString());
        LOGV2_WARNING(23723,
                      "Our set name did not match that of {requestTarget}",
                      message,
                      "requestTarget"_attr = request.target.toString());
        return;
    }

    if (!hbStatus.isOK() && hbStatus != ErrorCodes::InvalidReplicaSetConfig) {
        LOGV2_WARNING(
            23724,
            "Got error ({hbStatus}) response on heartbeat request to {requestTarget}; {hbResp}",
            "Got error response on heartbeat request",
            "hbStatus"_attr = hbStatus,
            "requestTarget"_attr = request.target,
            "hbResp"_attr = hbResp);
        _badResponses.push_back(std::make_pair(request.target, hbStatus));
        return;
    }

    if (_rsConfig->hasReplicaSetId()) {
        StatusWith<rpc::ReplSetMetadata> replMetadata =
            rpc::ReplSetMetadata::readFromMetadata(response.data);
        if (replMetadata.isOK() && replMetadata.getValue().getReplicaSetId().isSet() &&
            _rsConfig->getReplicaSetId() != replMetadata.getValue().getReplicaSetId()) {
            static constexpr char message[] =
                "Our replica set ID did not match that of our request target";
            _vetoStatus =
                Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                       str::stream() << message << ", replSetId: " << _rsConfig->getReplicaSetId()
                                     << ", requestTarget: " << request.target.toString()
                                     << ", requestTargetReplSetId: "
                                     << replMetadata.getValue().getReplicaSetId());
            LOGV2_WARNING(23726,
                          "Our replica set ID of {replSetId} did not match that of "
                          "{requestTarget}, which is {requestTargetId}",
                          message,
                          "replSetId"_attr = _rsConfig->getReplicaSetId(),
                          "requestTarget"_attr = request.target.toString(),
                          "requestTargetReplSetId"_attr =
                              replMetadata.getValue().getReplicaSetId());
        }
    }

    for (int i = 0; i < _rsConfig->getNumMembers(); ++i) {
        const MemberConfig& memberConfig = _rsConfig->getMemberAt(i);
        if (memberConfig.getHostAndPort() != request.target) {
            continue;
        }
        if (memberConfig.isElectable()) {
            ++_numElectable;
        }
        if (memberConfig.isVoter()) {
            _voters.push_back(request.target);
        }
        return;
    }
    MONGO_UNREACHABLE;
}

bool QuorumChecker::hasReceivedSufficientResponses() const {
    if (!_vetoStatus.isOK() || _numResponses == _rsConfig->getNumMembers()) {
        // Vetoed or everybody has responded.  All done.
        return true;
    }

    return false;
}

Status checkQuorumGeneral(executor::TaskExecutor* executor,
                          const ReplSetConfig& rsConfig,
                          const int myIndex,
                          long long term,
                          std::string logMessage) {
    auto checker = std::make_shared<QuorumChecker>(&rsConfig, myIndex, term);
    ScatterGatherRunner runner(checker, executor, std::move(logMessage));
    Status status = runner.run();
    if (!status.isOK()) {
        return status;
    }

    return checker->getFinalStatus();
}

Status checkQuorumForInitiate(executor::TaskExecutor* executor,
                              const ReplSetConfig& rsConfig,
                              const int myIndex,
                              long long term) {
    invariant(rsConfig.getConfigVersion() == 1);
    return checkQuorumGeneral(executor, rsConfig, myIndex, term, "initiate quorum check");
}

Status checkQuorumForReconfig(executor::TaskExecutor* executor,
                              const ReplSetConfig& rsConfig,
                              const int myIndex,
                              long long term) {
    invariant(rsConfig.getConfigVersion() > 1);
    return checkQuorumGeneral(executor, rsConfig, myIndex, term, "reconfig quorum check");
}

}  // namespace repl
}  // namespace mongo
