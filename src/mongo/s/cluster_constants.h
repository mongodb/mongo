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
        static const string shard;
        static const string database;
        static const string collection;
        static const string chunk;
        static const string tag;
        static const string mongos;
        static const string settings;
        static const string changelog;
        static const string locks;
        static const string lockpings;

        static const int version = 3;
    };

    /**
     * ShardFields holds all the field names and types for the shard collection.
     */
    struct ShardFields {
        static BSONField<string> name;       // shard's id
        static BSONField<string> host;       // connection string for the host(s)
        static BSONField<bool> draining;     // is it draining chunks?
        static BSONField<long long> maxSize; // max allowe disk space usage
    };

    /**
     * DatabaseFields holds all the field names and types for the database collection.
     */
    struct DatabaseFields {
        static BSONField<string> name;                  // database's name
        static BSONField<bool> partitioned;             // To be deprecated in 2.4
        static BSONField<string> primary;               // To be deprecated in 2.4

        static BSONField<string> DEPRECATED_name;       // last used in 1.4 series (version 2)
        static BSONField<bool> DEPRECATED_sharded;      // last used in 1.4 series

        // Being added in 2.4
        static BSONField<string> NEW_shard;             // primary shard for the database
        static BSONField<bool> NEW_draining;            // is the database being removed?
        static BSONField<bool> NEW_scatterCollections;  // distribute collection among shards
    };

    /**
     * CollectionFields holds all the field names and types for the collections collection.
     */
    struct CollectionFields {
        static BSONField<string> name;     // collection's name
        static BSONField<string> shard;    // primary, if not sharded
        static BSONField<BSONObj> key;     // sharding key, if sharded
        static BSONField<bool> unique;     // sharding key unique?
        static BSONField<Date_t> lastmod;  // when collecation was created
        static BSONField<bool> dropped;    // logical deletion
        static BSONField<bool> noBalance;  // true if balancing is disabled
    };

    /**
     * ChunkFields holds all the field names and types for the chunks collection.
     */
    struct ChunkFields {
        static BSONField<string> name;            // chunk's id
        static BSONField<string> ns;              // namespace this collection is in
        static BSONField<BSONObj> min;            // first key of the chunk, including
        static BSONField<BSONObj> max;            // last key of the chunk, non-including
        static BSONField<string> lastmod;         // major | minor versions
        static BSONField<string> shard;           // home of this chunk
        static BSONField<bool> jumbo;             // too big to move?

        // Transition to new format, 2.2 -> 2.4
        // 2.2 can read both lastmod + lastmodEpoch format and 2.4 [ lastmod, OID ] formats.
        static BSONField<OID> lastmodEpoch;       // OID, to disambiguate collection incarnations

        // Being added in 2.4
        // This will deprecate lastmod + lastmodEpoch format.
        static BSONField<BSONArray> NEW_lastmod;  // [Date_t, OID] format
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

    // ============  below not yet hooked  ============

    /**
     * MongosFields holds all the field names and types for the mongos collection.
     */
    struct MongosFields {
        static BSONField<string> UNHOOKED_name;  // process id string
        static BSONField<Date_t> UNHOOKED_ping;  // last time it was seen alive
        static BSONField<int> UNHOOKED_up;       // uptime at the last ping
        static BSONField<bool> UNHOOKED_waiting; // for testing purposes
    };

    /**
     * SettingFields holds all the field names and types for the settings collection.
     */
    struct SettingFields {
        static BSONField<int> UNHOOKED_name;     // key for the parameter
        static BSONField<string> UNHOOKED_value; // value for the parameter
    };

    /**
     * ChangelogFields holds all the field names and types for the changelog collection.
     */
    struct ChangelogFields {
        static BSONField<string> UNHOOKED_name;
        static BSONField<string> UNHOOKED_server;
        static BSONField<string> UNHOOKED_clientAddr;
        static BSONField<Date_t> UNHOOKED_time;
        static BSONField<string> UNHOOKED_what;
        static BSONField<string> UNHOOKED_ns;
        static BSONField<string> UNHOOKED_details;
    };

    /**
     * LockFields holds all the field names and types for the locks collection.
     */
    struct LockFields {

        // name of the lock
        static BSONField<string> name;

        // 0: Unlocked | 1: Locks in contention | 2: Lock held
        static BSONField<int> state;

        // the process field contains the (unique) identifier for the instance
        // of mongod/mongos which has requested the lock
        static BSONField<string> process;

        // a unique identifier for the instance of the lock itself. Allows for
        // safe cleanup after network partitioning
        static BSONField<OID> lockID;

        // a note about why the lock is held, or which subcomponent is holding it
        static BSONField<string> who;

        // a human readable description of the purpose of the lock
        static BSONField<string> why;
    };

    /**
     * LockPingFields holds all the field names and types for the lockpings collection.
     */
    struct LockPingFields {
        static BSONField<string> UNHOOKED_name; // process id holding the lock
        static BSONField<Date_t> UNHOOKED_ping; // last time it pinged
    };

} // namespace mongo
