/**
 *    Copyright 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/freshness_scanner.h"

#include "mongo/base/status.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using executor::RemoteCommandRequest;

FreshnessScanner::Algorithm::Algorithm(const ReplicaSetConfig& rsConfig,
                                       int myIndex,
                                       Milliseconds timeout)
    : _rsConfig(rsConfig), _myIndex(myIndex), _timeout(timeout) {
    for (int index = 0; index < _rsConfig.getNumMembers(); index++) {
        if (index != _myIndex) {
            _targets.push_back(_rsConfig.getMemberAt(index).getHostAndPort());
        }
    }
    _totalRequests = _targets.size();
}

std::vector<RemoteCommandRequest> FreshnessScanner::Algorithm::getRequests() const {
    BSONObjBuilder cmdBuilder;
    cmdBuilder << "replSetGetStatus" << 1;
    const BSONObj getStatusCmd = cmdBuilder.obj();

    std::vector<RemoteCommandRequest> requests;
    for (auto& target : _targets) {
        requests.push_back(RemoteCommandRequest(target, "admin", getStatusCmd, _timeout));
    }
    return requests;
}

void FreshnessScanner::Algorithm::processResponse(const RemoteCommandRequest& request,
                                                  const ResponseStatus& response) {
    _responsesProcessed++;
    if (!response.isOK()) {  // failed response
        LOG(2) << "FreshnessScanner: Got failed response from " << request.target << ": "
               << response.getStatus();
    } else {
        BSONObj opTimesObj = response.getValue().data.getObjectField("optimes");
        OpTime lastOpTime;
        Status status = bsonExtractOpTimeField(opTimesObj, "appliedOpTime", &lastOpTime);
        if (!status.isOK()) {
            return;
        }

        int index = _rsConfig.findMemberIndexByHostAndPort(request.target);
        FreshnessInfo freshnessInfo{index, lastOpTime};

        auto cmp = [](const FreshnessInfo& a, const FreshnessInfo& b) {
            return a.opTime > b.opTime;
        };
        auto iter =
            std::upper_bound(_freshnessInfos.begin(), _freshnessInfos.end(), freshnessInfo, cmp);
        _freshnessInfos.insert(iter, freshnessInfo);
    }
}

bool FreshnessScanner::Algorithm::hasReceivedSufficientResponses() const {
    return _responsesProcessed == _totalRequests;
}

FreshnessScanner::Result FreshnessScanner::Algorithm::getResult() const {
    invariant(hasReceivedSufficientResponses());
    return _freshnessInfos;
}

StatusWith<ReplicationExecutor::EventHandle> FreshnessScanner::start(
    ReplicationExecutor* executor,
    const ReplicaSetConfig& rsConfig,
    int myIndex,
    Milliseconds timeout) {
    _algorithm.reset(new Algorithm(rsConfig, myIndex, timeout));
    _runner.reset(new ScatterGatherRunner(_algorithm.get(), executor));
    return _runner->start();
}

void FreshnessScanner::cancel() {
    _runner->cancel();
}

FreshnessScanner::Result FreshnessScanner::getResult() const {
    return _algorithm->getResult();
}

}  // namespace repl
}  // namespace mongo
