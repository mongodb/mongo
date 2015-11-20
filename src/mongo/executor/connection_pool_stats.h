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

#include <unordered_map>

#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace executor {

/**
 * Holds connection information for a specific remote host. These objects are maintained by
 * a parent ConnectionPoolStats object and should not need to be created directly.
 */
struct ConnectionStatsPerHost {
    ConnectionStatsPerHost(size_t nInUse, size_t nAvailable, size_t nCreated);

    ConnectionStatsPerHost();

    ConnectionStatsPerHost& operator+=(const ConnectionStatsPerHost& other);

    size_t inUse = 0u;
    size_t available = 0u;
    size_t created = 0u;
};

/**
 * Aggregates connection information for the connPoolStats command. Connection pools should
 * use the updateStatsForHost() method to append their host-specific information to this object.
 * Total connection counts will then be updated accordingly.
 */
struct ConnectionPoolStats {
    void updateStatsForHost(HostAndPort host, ConnectionStatsPerHost newStats);

    void appendToBSON(mongo::BSONObjBuilder& result);

    size_t totalInUse = 0u;
    size_t totalAvailable = 0u;
    size_t totalCreated = 0u;

    std::unordered_map<HostAndPort, ConnectionStatsPerHost> statsByHost;
};

}  // namespace executor
}  // namespace mongo
