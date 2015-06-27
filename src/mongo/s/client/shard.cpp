/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#include "mongo/s/client/shard.h"

#include <string>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::stringstream;

Shard::Shard(const ShardId& id,
             const ConnectionString& connStr,
             std::unique_ptr<RemoteCommandTargeter> targeter)
    : _id(id), _cs(connStr), _targeter(std::move(targeter)) {}

Shard::~Shard() = default;

ShardPtr Shard::lookupRSName(const string& name) {
    return grid.shardRegistry()->lookupRSName(name);
}

ShardStatus Shard::getStatus() const {
    const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet::primaryOnly());
    auto shardHost = uassertStatusOK(getTargeter()->findHost(readPref));

    // List databases command
    BSONObj listDatabases = uassertStatusOK(
        grid.shardRegistry()->runCommand(shardHost, "admin", BSON("listDatabases" << 1)));
    BSONElement totalSizeElem = listDatabases["totalSize"];
    uassert(28590, "totalSize field not found in listDatabases", totalSizeElem.isNumber());

    // Server status command
    BSONObj serverStatus = uassertStatusOK(
        grid.shardRegistry()->runCommand(shardHost, "admin", BSON("serverStatus" << 1)));
    BSONElement versionElement = serverStatus["version"];
    uassert(28599, "version field not found in serverStatus", versionElement.type() == String);

    return ShardStatus(totalSizeElem.numberLong(), versionElement.str());
}

std::string Shard::toString() const {
    return _id + ":" + _cs.toString();
}

void Shard::reloadShardInfo() {
    grid.shardRegistry()->reload();
}

void Shard::removeShard(const ShardId& id) {
    grid.shardRegistry()->remove(id);
}

ShardStatus::ShardStatus(long long dataSizeBytes, const string& mongoVersion)
    : _dataSizeBytes(dataSizeBytes), _mongoVersion(mongoVersion) {}

std::string ShardStatus::toString() const {
    return str::stream() << " dataSizeBytes: " << _dataSizeBytes << " version: " << _mongoVersion;
}

bool ShardStatus::operator<(const ShardStatus& other) const {
    return dataSizeBytes() < other.dataSizeBytes();
}

}  // namespace mongo
