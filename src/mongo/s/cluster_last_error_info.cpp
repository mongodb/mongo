/*
 *    Copyright (C) 2008 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/cluster_last_error_info.h"

#include <utility>

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/util/log.h"

namespace mongo {

    const ClientBasic::Decoration<ClusterLastErrorInfo> ClusterLastErrorInfo::get =
        ClientBasic::declareDecoration<ClusterLastErrorInfo>();

    void ClusterLastErrorInfo::addShardHost(const std::string& shardHost) {
        _cur->shardHostsWritten.insert( shardHost );
    }

    void ClusterLastErrorInfo::addHostOpTime(ConnectionString connStr, HostOpTime stat) {
        _cur->hostOpTimes[connStr] = stat;
    }

    void ClusterLastErrorInfo::addHostOpTimes( const HostOpTimeMap& hostOpTimes ) {
        for ( HostOpTimeMap::const_iterator it = hostOpTimes.begin();
            it != hostOpTimes.end(); ++it ) {
            addHostOpTime(it->first, it->second);
        }
    }

    void ClusterLastErrorInfo::newRequest() {
        std::swap(_cur, _prev);
        _cur->clear();
    }

    void ClusterLastErrorInfo::disableForCommand() {
        RequestInfo* temp = _cur;
        _cur = _prev;
        _prev = temp;
    }

    static TimerStats gleWtimeStats;
    static ServerStatusMetricField<TimerStats> displayGleLatency("getLastError.wtime",
                                                                 &gleWtimeStats);

    void saveGLEStats(const BSONObj& result, const std::string& hostString) {
        if (!haveClient()) {
            return;
        }
        if (result[kGLEStatsFieldName].type() != Object) {
            return;
        }
        std::string errmsg;
        ConnectionString shardConn = ConnectionString::parse(hostString, errmsg);

        BSONElement subobj = result[kGLEStatsFieldName];
        Timestamp lastOpTime = subobj[kGLEStatsLastOpTimeFieldName].timestamp();
        OID electionId = subobj[kGLEStatsElectionIdFieldName].OID();
        auto& clientInfo = cc();
        LOG(4) << "saveGLEStats lastOpTime:" << lastOpTime 
               << " electionId:" << electionId;

        ClusterLastErrorInfo::get(clientInfo).addHostOpTime(
                shardConn, HostOpTime(lastOpTime, electionId));
    }

} // namespace mongo
