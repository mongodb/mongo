/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/repl/repl_set_heartbeat_response.h"

#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

    ReplSetHeartbeatResponse::ReplSetHeartbeatResponse() :
            _electionTimeSet(false),
            _timeSet(false),
            _opTimeSet(false),
            _electableSet(false),
            _electable(false),
            _mismatch(false),
            _isReplSet(false),
            _stateDisagreement(false),
            _state(-1),
            _version(-1),
            _setName(""),
            _hbmsg(""),
            _syncingTo("")
            {}

    void ReplSetHeartbeatResponse::addToBSON(BSONObjBuilder* builder) const {
        if (_opTimeSet) {
            builder->appendDate("opTime", _opTime);
        }
        if (_timeSet) {
            long long millis = _time.asInt64();
            *builder << "time" << millis;
        }
        if (_electionTimeSet) {
            builder->appendDate("electionTime", _electionTime);
        }
        if (_config.isInitialized()) {
            *builder << "config" << _config.toBSON();
        }
        if (_electableSet) {
            *builder << "e" << _electable;
        }
        if (_mismatch) {
            *builder << "mismatch" << _mismatch;
        }
        if (_isReplSet) {
            *builder << "rs" << _isReplSet;
        }
        if (_stateDisagreement) {
            *builder << "stateDisagreement" << _stateDisagreement;
        }
        if (_state != -1) {
            *builder << "state" << _state;
        }
        if (_version != -1) {
            *builder << "v" << _version;
        }
        *builder << "hbmsg" << _hbmsg;
        if (!_setName.empty()) {
            *builder << "set" << _setName;
        }
        if (!_syncingTo.empty()) {
            *builder << "syncingTo" << _syncingTo;
        }
    }

    BSONObj ReplSetHeartbeatResponse::toBSON() const {
        BSONObjBuilder builder;
        addToBSON(&builder);
        return builder.done();
    }

} // namespace repl
} // namespace mongo
