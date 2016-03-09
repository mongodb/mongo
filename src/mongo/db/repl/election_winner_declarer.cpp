/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/db/repl/election_winner_declarer.h"

#include "mongo/base/status.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using executor::RemoteCommandRequest;

ElectionWinnerDeclarer::Algorithm::Algorithm(const std::string& setName,
                                             long long winnerId,
                                             long long term,
                                             const std::vector<HostAndPort>& targets)
    : _setName(setName), _winnerId(winnerId), _term(term), _targets(targets) {}

ElectionWinnerDeclarer::Algorithm::~Algorithm() {}

std::vector<RemoteCommandRequest> ElectionWinnerDeclarer::Algorithm::getRequests() const {
    BSONObjBuilder declareElectionWinnerCmdBuilder;
    declareElectionWinnerCmdBuilder.append("replSetDeclareElectionWinner", 1);
    declareElectionWinnerCmdBuilder.append("setName", _setName);
    declareElectionWinnerCmdBuilder.append("winnerId", _winnerId);
    declareElectionWinnerCmdBuilder.append("term", _term);
    const BSONObj declareElectionWinnerCmd = declareElectionWinnerCmdBuilder.obj();

    std::vector<RemoteCommandRequest> requests;
    for (const auto& target : _targets) {
        requests.push_back(RemoteCommandRequest(
            target,
            "admin",
            declareElectionWinnerCmd,
            Milliseconds(30 * 1000)));  // trying to match current Socket timeout
    }

    return requests;
}

void ElectionWinnerDeclarer::Algorithm::processResponse(const RemoteCommandRequest& request,
                                                        const ResponseStatus& response) {
    _responsesProcessed++;
    if (!response.isOK()) {  // failed response
        log() << "ElectionWinnerDeclarer: Got failed response from " << request.target << ": "
              << response.getStatus();
        return;
    }

    Status cmdResponseStatus = getStatusFromCommandResult(response.getValue().data);
    if (!cmdResponseStatus.isOK()) {  // disagreement response
        _failed = true;
        _status = cmdResponseStatus;
        log() << "ElectionWinnerDeclarer: Got error response from " << request.target
              << " with term: " << response.getValue().data["term"].Number()
              << " and error: " << cmdResponseStatus;
    }
}

bool ElectionWinnerDeclarer::Algorithm::hasReceivedSufficientResponses() const {
    return _failed || _responsesProcessed == static_cast<int>(_targets.size());
}

ElectionWinnerDeclarer::ElectionWinnerDeclarer() : _isCanceled(false) {}
ElectionWinnerDeclarer::~ElectionWinnerDeclarer() {}

StatusWith<ReplicationExecutor::EventHandle> ElectionWinnerDeclarer::start(
    ReplicationExecutor* executor,
    const std::string& setName,
    long long winnerId,
    long long term,
    const std::vector<HostAndPort>& targets,
    const stdx::function<void()>& onCompletion) {
    _algorithm.reset(new Algorithm(setName, winnerId, term, targets));
    _runner.reset(new ScatterGatherRunner(_algorithm.get(), executor));
    return _runner->start();
}

void ElectionWinnerDeclarer::cancel() {
    _isCanceled = true;
    _runner->cancel();
}

Status ElectionWinnerDeclarer::getStatus() const {
    return _algorithm->getStatus();
}

}  // namespace repl
}  // namespace mongo
