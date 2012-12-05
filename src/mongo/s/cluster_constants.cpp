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

    const string ConfigNS::changelog = "config.changelog";
    BSONField<string> ChangelogFields::changeID("_id");
    BSONField<string> ChangelogFields::server("server");
    BSONField<string> ChangelogFields::clientAddr("clientAddr");
    BSONField<Date_t> ChangelogFields::time("time");
    BSONField<string> ChangelogFields::what("what");
    BSONField<string> ChangelogFields::ns("ns");
    BSONField<BSONObj> ChangelogFields::details("details");

    const string ConfigNS::lockpings = "config.lockpings";
    BSONField<string> LockPingFields::process("_id");
    BSONField<Date_t> LockPingFields::ping("ping");

} // namespace mongo
