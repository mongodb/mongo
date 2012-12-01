/**
 *    Copyright (C) 2012 10gen Inc.
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
 */

#include "mongo/s/cluster_constants.h"

namespace mongo {

    const string ConfigNS::database = "config.databases";
    BSONField<string> DatabaseFields::name("_id");
    BSONField<bool> DatabaseFields::partitioned("partitioned");
    BSONField<string> DatabaseFields::primary("primary");
    BSONField<string> DatabaseFields::DEPRECATED_name("name");
    BSONField<bool> DatabaseFields::DEPRECATED_sharded("sharded");
    BSONField<string> DatabaseFields::NEW_shard("shard");
    BSONField<bool> DatabaseFields::NEW_draining("draining");
    BSONField<bool> DatabaseFields::NEW_scatterCollections("scatterCollections");

    const string ConfigNS::tag = "config.tags";
    BSONField<string> TagFields::ns("ns");
    BSONField<string> TagFields::tag("tag");
    BSONField<BSONObj> TagFields::min("min");
    BSONField<BSONObj> TagFields::max("max");

    const string ConfigNS::mongos = "config.mongos";
    BSONField<string> MongosFields::name("_id");
    BSONField<Date_t> MongosFields::ping("ping");
    BSONField<int> MongosFields::up("up");
    BSONField<bool> MongosFields::waiting("waiting");

    const string ConfigNS::settings = "config.settings";
    BSONField<string> SettingsFields::key("_id");
    BSONField<int> SettingsFields::chunksize("value");
    BSONField<bool> SettingsFields::balancerStopped("stopped");
    BSONField<BSONObj> SettingsFields::balancerActiveWindow("activeWindow");
    BSONField<bool> SettingsFields::shortBalancerSleep("_nosleep");
    BSONField<bool> SettingsFields::secondaryThrottle("_secondaryThrottle");

    const string ConfigNS::changelog = "config.changelog";
    BSONField<string> ChangelogFields::changeID("_id");
    BSONField<string> ChangelogFields::server("server");
    BSONField<string> ChangelogFields::clientAddr("clientAddr");
    BSONField<Date_t> ChangelogFields::time("time");
    BSONField<string> ChangelogFields::what("what");
    BSONField<string> ChangelogFields::ns("ns");
    BSONField<BSONObj> ChangelogFields::details("details");

    const string ConfigNS::locks = "config.locks";
    BSONField<string> LockFields::name("_id");
    BSONField<int> LockFields::state("state");
    BSONField<string> LockFields::process("process");
    BSONField<OID> LockFields::lockID("ts");
    BSONField<string> LockFields::who("who");
    BSONField<string> LockFields::why("why");

    const string ConfigNS::lockpings = "config.lockpings";
    BSONField<string> LockPingFields::process("_id");
    BSONField<Date_t> LockPingFields::ping("ping");

} // namespace mongo
