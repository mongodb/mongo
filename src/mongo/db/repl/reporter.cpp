/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/repl/reporter.h"
#include "mongo/util/log.h"

#include "mongo/db/repl/replication_executor.h"

namespace mongo {
namespace repl {

    ReplicationProgressManager::~ReplicationProgressManager() {}

    Reporter::Reporter(ReplicationExecutor* executor,
                       ReplicationProgressManager* ReplicationProgressManager,
                       const HostAndPort& target)
        : _executor(executor),
          _updatePositionSource(ReplicationProgressManager),
          _target(target),
          _status(Status::OK()),
          _willRunAgain(false),
          _active(false) {

        uassert(ErrorCodes::BadValue, "null replication executor", executor);
        uassert(ErrorCodes::BadValue,
                "null replication progress manager",
                ReplicationProgressManager);
        uassert(ErrorCodes::BadValue, "target name cannot be empty", !target.empty());
    }

    Reporter::~Reporter() {
        cancel();
    }

    void Reporter::cancel() {
        boost::lock_guard<boost::mutex> lk(_mutex);

        if (!_active) {
            return;
        }

        _status = Status(ErrorCodes::CallbackCanceled, "Reporter no longer valid");
        _active = false;
        _willRunAgain = false;
        invariant(_remoteCommandCallbackHandle.isValid());
        _executor->cancel(_remoteCommandCallbackHandle);
    }

    Status Reporter::trigger() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _schedule_inlock();
    }

    Status Reporter::_schedule_inlock() {
        if (!_status.isOK()) {
            return _status;
        }

        if (_active) {
            _willRunAgain = true;
            return _status;
        }

        LOG(2) << "Reporter scheduling report to : " << _target;

        _willRunAgain = false;

        BSONObjBuilder cmd;
        _updatePositionSource->prepareReplSetUpdatePositionCommand(&cmd);
        StatusWith<ReplicationExecutor::CallbackHandle> scheduleResult =
            _executor->scheduleRemoteCommand(
                ReplicationExecutor::RemoteCommandRequest(_target, "admin", cmd.obj()),
                stdx::bind(&Reporter::_callback, this, stdx::placeholders::_1));

        if (!scheduleResult.isOK()) {
            _status = scheduleResult.getStatus();
            LOG(2) << "Reporter failed to schedule with status: " << _status;

            return _status;
        }

        _active = true;
        _remoteCommandCallbackHandle = scheduleResult.getValue();
        return Status::OK();
    }

    void Reporter::_callback(const ReplicationExecutor::RemoteCommandCallbackData& rcbd) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _status = rcbd.response.getStatus();
        _active = false;

        LOG(2) << "Reporter ended with status: " << _status << " after reporting to " << _target;
        if (_status.isOK() && _willRunAgain) {
            _schedule_inlock();
        }
        else {
            _willRunAgain = false;
        }
    }

    Status Reporter::previousReturnStatus() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _status;
    }

    bool Reporter::isActive() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _active;
    }

    bool Reporter::willRunAgain() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _willRunAgain;
    }
} // namespace repl
} // namespace mongo
