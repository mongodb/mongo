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

#pragma once

#include <string>

#include "mongo/bson/util/misc.h"  // for Date_t
#include "mongo/db/jsobj.h"        // for BSON_Field and dependencies

namespace mongo {

    using std::string;

    /**
     * ConfigNS holds the names for all the metadata collections stored in a config server.
     */
    struct ConfigNS {
        static const string tag;
        static const string mongos;
        static const string changelog;
        static const string locks;
        static const string lockpings;

        static const int version = 3;
    };

    /**
     * TagFields holds all the field names and types for the tags collection.
     */
    struct TagFields {
        static BSONField<string> ns;    // namespace this tag is for
        static BSONField<string> tag;   // tag name
        static BSONField<BSONObj> min;  // first key of the tag, including
        static BSONField<BSONObj> max;  // last key of the tag, non-including
    };

    /**
     * MongosFields holds all the field names and types for the mongos collection.
     */
    struct MongosFields {

        // "host:port" for this mongos
        static BSONField<string> name;

        // last time it was seen alive
        static BSONField<Date_t> ping;

        // uptime at the last ping
        static BSONField<int> up;

        // for testing purposes
        static BSONField<bool> waiting;
    };

    /**
     * ChangelogFields holds all the field names and types for the changelog collection.
     */
    struct ChangelogFields {

        // id for this change "<hostname>-<current_time>-<increment>"
        static BSONField<string> changeID;

        // hostname of server that we are making the change on.  Does not include port.
        static BSONField<string> server;

        // hostname:port of the client that made this change
        static BSONField<string> clientAddr;

        // time this change was made
        static BSONField<Date_t> time;

        // description of the change
        static BSONField<string> what;

        // database or collection this change applies to
        static BSONField<string> ns;

        // A BSONObj containing extra information about some operations
        static BSONField<BSONObj> details;
    };

    /**
     * LockPingFields holds all the field names and types for the lockpings collection.
     */
    struct LockPingFields {

        // string describing the process holding the lock
        static BSONField<string> process;

        // last time the holding process updated this document
        static BSONField<Date_t> ping;
    };

} // namespace mongo
