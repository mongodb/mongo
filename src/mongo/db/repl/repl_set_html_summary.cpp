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

#include "mongo/db/repl/repl_set_html_summary.h"

#include <sstream>
#include <string>

#include "mongo/util/mongoutils/html.h"
#include "mongo/util/mongoutils/str.h"


namespace mongo {
namespace repl {

ReplSetHtmlSummary::ReplSetHtmlSummary() : _selfIndex(-1), _primaryIndex(-1), _selfUptime(0) {}

namespace {

/**
 * Turns an unsigned int representing a duration of time in milliseconds and turns it into
 * a human readable time string representation.
 */
std::string ago(unsigned int duration) {
    std::stringstream s;
    if (duration < 180) {
        s << duration << " sec";
        if (duration != 1)
            s << 's';
    } else if (duration < 3600) {
        s.precision(2);
        s << duration / 60.0 << " mins";
    } else {
        s.precision(2);
        s << duration / 3600.0 << " hrs";
    }
    return s.str();
}

unsigned int timeDifference(Date_t now, Date_t past) {
    return static_cast<unsigned int>(past != Date_t() ? durationCount<Seconds>(now - past) : 0);
}

std::string stateAsHtml(const MemberState& s) {
    using namespace html;

    if (s.s == MemberState::RS_STARTUP)
        return a("", "server still starting up, or still trying to initiate the set", "STARTUP");
    if (s.s == MemberState::RS_PRIMARY)
        return a("", "this server thinks it is primary", "PRIMARY");
    if (s.s == MemberState::RS_SECONDARY)
        return a("", "this server thinks it is a secondary (slave mode)", "SECONDARY");
    if (s.s == MemberState::RS_RECOVERING)
        return a("",
                 "recovering/resyncing; after recovery usually auto-transitions to secondary",
                 "RECOVERING");
    if (s.s == MemberState::RS_STARTUP2)
        return a("", "loaded config, still determining who is primary", "STARTUP2");
    if (s.s == MemberState::RS_ARBITER)
        return a("", "this server is an arbiter only", "ARBITER");
    if (s.s == MemberState::RS_DOWN)
        return a("", "member is down, slow, or unreachable", "DOWN");
    if (s.s == MemberState::RS_ROLLBACK)
        return a("", "rolling back operations to get in sync", "ROLLBACK");
    if (s.s == MemberState::RS_UNKNOWN)
        return a("", "we do not know what state this node is in", "UNKNOWN");
    if (s.s == MemberState::RS_REMOVED)
        return a("", "this server has been removed from the replica set config", "ROLLBACK");
    return "";
}
}

const std::string ReplSetHtmlSummary::toHtmlString() const {
    using namespace html;

    std::stringstream s;

    if (!_config.isInitialized()) {
        s << p("Still starting up, or else replset is not yet initiated.");
        return s.str();
    }
    if (_selfIndex < 0) {
        s << p(
            "This node is not a member of its replica set configuration, it most likely was"
            " removed recently");
        return s.str();
    }

    int votesUp = 0;
    int totalVotes = 0;
    // Build table of node information.
    std::stringstream memberTable;
    const char* h[] = {
        "Member",
        "<a title=\"member id in the replset config\">id</a>",
        "Up",
        "<a title=\"length of time we have been continuously connected to the other member "
        "with no reconnects (for self, shows uptime)\">cctime</a>",
        "<a title=\"when this server last received a heartbeat response - includes error code "
        "responses\">Last heartbeat</a>",
        "Votes",
        "Priority",
        "State",
        "Messages",
        "<a title=\"how up to date this server is.  this value polled every few seconds so "
        "actually lag is typically lower than value shown here.\">optime</a>",
        0};
    memberTable << table(h);

    for (int i = 0; i < _config.getNumMembers(); ++i) {
        const MemberConfig& memberConfig = _config.getMemberAt(i);
        const MemberHeartbeatData& memberHB = _hbData[i];
        bool isSelf = _selfIndex == i;
        bool up = memberHB.getHealth() > 0;

        totalVotes += memberConfig.getNumVotes();
        if (up || isSelf) {
            votesUp += memberConfig.getNumVotes();
        }

        memberTable << tr();
        if (isSelf) {
            memberTable << td(memberConfig.getHostAndPort().toString() + " (me)");
            memberTable << td(memberConfig.getId());
            memberTable << td("1");  // up
            memberTable << td(ago(_selfUptime));
            memberTable << td("");  // last heartbeat
            memberTable << td(std::to_string(memberConfig.getNumVotes()));
            memberTable << td(std::to_string(memberConfig.getPriority()));
            memberTable << td(stateAsHtml(_selfState) +
                              (memberConfig.isHidden() ? " (hidden)" : ""));
            memberTable << td(_selfHeartbeatMessage);
            memberTable << td(_selfOptime.toString());
        } else {
            std::stringstream link;
            link << "http://" << memberConfig.getHostAndPort().host() << ':'
                 << (memberConfig.getHostAndPort().port() + 1000) << "/_replSet";
            memberTable << td(a(link.str(), "", memberConfig.getHostAndPort().toString()));
            memberTable << td(memberConfig.getId());
            memberTable << td(red(str::stream() << memberHB.getHealth(), !up));
            const unsigned int uptime = timeDifference(_now, memberHB.getUpSince());
            memberTable << td(ago(uptime));
            if (memberHB.getLastHeartbeat() == Date_t()) {
                memberTable << td("never");
            } else {
                memberTable << td(ago(timeDifference(_now, memberHB.getLastHeartbeat())));
            }
            memberTable << td(std::to_string(memberConfig.getNumVotes()));
            memberTable << td(std::to_string(memberConfig.getPriority()));
            std::string state =
                memberHB.getState().toString() + (memberConfig.isHidden() ? " (hidden)" : "");
            if (up) {
                memberTable << td(state);
            } else {
                memberTable << td(grey(str::stream() << "(was " << state << ')', true));
            }
            memberTable << td(grey(memberHB.getLastHeartbeatMsg(), !up));
            // TODO(dannenberg): change timestamp to optime in V1
            memberTable << td(memberHB.getLastHeartbeat() == Date_t()
                                  ? "?"
                                  : memberHB.getAppliedOpTime().toString());
        }
        memberTable << _tr();
    }
    memberTable << _table();

    s << table(0, false);
    s << tr("Set name:", _config.getReplSetName());
    bool majorityUp = votesUp * 2 > totalVotes;
    s << tr("Majority up:", majorityUp ? "yes" : "no");

    const MemberConfig& selfConfig = _config.getMemberAt(_selfIndex);

    if (_primaryIndex >= 0 && _primaryIndex != _selfIndex && !selfConfig.isArbiter()) {
        int lag = _hbData[_primaryIndex].getAppliedOpTime().getTimestamp().getSecs() -
            _selfOptime.getTimestamp().getSecs();
        s << tr("Lag: ", str::stream() << lag << " secs");
    }

    s << _table();

    s << memberTable.str();

    return s.str();
}

}  // namespace repl
}  // namespace mongo
