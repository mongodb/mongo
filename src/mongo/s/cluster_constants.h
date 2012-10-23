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

#include "mongo/db/jsobj.h" // for BSONField

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
        static const string tags;
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

    // ============  below not yet hooked  ============

    /**
     * DatabaseFields holds all the field names and types for the database collection.
     */
    struct DatabaseFields {
        static BSONField<string> UNHOOKED_name;             // database's name
        static BSONField<string> UNHOOKED_shard;            // primary shard for the database
        static BSONField<bool> UNHOOKED_draining;           // is the database being removed?
        static BSONField<bool> UNHOOKED_scatterCollections; // distribute collection among shards

        static BSONField<bool> DEPRECATED_partitioned;      // last used in 2.2 series
        static BSONField<string> DEPRECATED_oldShard;       // last used in 2.2 series
    };

    /**
     * CollectionFields holds all the field names and types for the collections collection.
     */
    struct CollectionFields {
        static BSONField<string> UNHOOKED_name;       // collection's name
        static BSONField<string> UNHOOKED_shard;      // primary, if not sharded
        static BSONField<BSONObj> UNHOOKED_shardKey;  // shardkey, if sharded
        static BSONField<string> UNHOOKED_epoch;      // collection's incarnation

        static BSONField<string> DEPRECATED_oldEpoch; // last used in 2.2 series
        static BSONField<bool> DEPRECATED_dropped;    // last used in 2.2 series
        static BSONField<bool> DEPRECATED_unique;     // last used in 2.2 series
    };

    /**
     * ChunkFields holds all the field names and types for the chunks collection.
     */
    struct ChunkFields {
        static BSONField<string> UNHOOKED_name;          // chunk's id
        static BSONField<BSONObj> UNHOOKED_min;          // first key of the chunk, including
        static BSONField<BSONObj> UNHOOKED_max;          // last key of the chunk, non-including
        static BSONField<string> UNHOOKED_version;       // major | minor
        static BSONField<string> UNHOOKED_shard;         // home of this chunk
        static BSONField<bool> UNHOOKED_jumbo;           // too big to move?

        static BSONField<string> DEPRECATED_oldVersion;  // last used in 2.2 series
        static BSONField<string> DEPRECATED_epoch;       // last used in 2.2 series
    };

    /**
     * TagFields holds all the field names and types for the tags collection.
     */
    struct TagFields {
        static BSONField<string> UNHOOKED_tag;   // tag name
        static BSONField<BSONObj> UNHOOKED_min;  // first key of the tag, including
        static BSONField<BSONObj> UNHOOKED_max;  // last key of the tag, non-including
    };

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
        static BSONField<string> UNHOOKED_name; // process id holding the lock
        static BSONField<int> UNHOOKED_state;   // 0: | 1: | 2:
        static BSONField<Date_t> UNHOOKED_ts;
        static BSONField<string> UNHOOKED_who;
    };

    /**
     * LockPingFields holds all the field names and types for the lockpings collection.
     */
    struct LockPingFields {
        static BSONField<string> UNHOOKED_name; // process id holding the lock
        static BSONField<Date_t> UNHOOKED_ping; // last time it pinged
    };

} // namespace mongo
