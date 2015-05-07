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

#include "mongo/db/repl/read_after_optime_response.h"

#include "mongo/bson/bsonobjbuilder.h"

using std::string;

namespace mongo {
namespace repl {

    const string ReadAfterOpTimeResponse::kWaitedMSFieldName("waitedMS");

    ReadAfterOpTimeResponse::ReadAfterOpTimeResponse(Status status):
        ReadAfterOpTimeResponse(status, stdx::chrono::milliseconds(0), false) {
    }

    ReadAfterOpTimeResponse::ReadAfterOpTimeResponse():
        ReadAfterOpTimeResponse(Status::OK()) {
    }

    ReadAfterOpTimeResponse::ReadAfterOpTimeResponse(Status status,
                                                     stdx::chrono::milliseconds duration):
        ReadAfterOpTimeResponse(status, duration, true) {
    }

    ReadAfterOpTimeResponse::ReadAfterOpTimeResponse(Status status,
                                                     stdx::chrono::milliseconds duration,
                                                     bool waited):
        _waited(waited),
        _duration(duration),
        _status(status) {
    }

    void ReadAfterOpTimeResponse::appendInfo(BSONObjBuilder* builder) {
        if (!_waited) {
            return;
        }

        builder->append(kWaitedMSFieldName, durationCount<Milliseconds>(_duration));
    }

    bool ReadAfterOpTimeResponse::didWait() const {
        return _waited;
    }

    stdx::chrono::milliseconds ReadAfterOpTimeResponse::getDuration() const {
        return _duration;
    }

    Status ReadAfterOpTimeResponse::getStatus() const {
        return _status;
    }

} // namespace repl
} // namespace mongo
