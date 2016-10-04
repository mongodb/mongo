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

#pragma once

#include <string>
#include <vector>

#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replica_set_config.h"

namespace mongo {

namespace repl {

/**
 * Class containing all the information needed to build the replSet page on http interface,
 * and the logic to generate that page.
 */
class ReplSetHtmlSummary {
public:
    ReplSetHtmlSummary();

    const std::string toHtmlString() const;

    void setConfig(const ReplicaSetConfig& config) {
        _config = config;
    }

    void setHBData(const std::vector<MemberHeartbeatData>& hbData) {
        _hbData = hbData;
    }

    void setSelfIndex(int index) {
        _selfIndex = index;
    }

    void setPrimaryIndex(int index) {
        _primaryIndex = index;
    }

    void setSelfOptime(const OpTime& ts) {
        _selfOptime = ts;
    }

    void setSelfUptime(unsigned int time) {
        _selfUptime = time;
    }

    void setNow(Date_t now) {
        _now = now;
    }

    void setSelfState(const MemberState& state) {
        _selfState = state;
    }

    void setSelfHeartbeatMessage(StringData msg) {
        _selfHeartbeatMessage = msg.toString();
    }

private:
    ReplicaSetConfig _config;
    std::vector<MemberHeartbeatData> _hbData;
    Date_t _now;
    int _selfIndex;
    int _primaryIndex;
    OpTime _selfOptime;
    unsigned int _selfUptime;
    MemberState _selfState;
    std::string _selfHeartbeatMessage;
};

}  // namespace repl
}  // namespace mongo
