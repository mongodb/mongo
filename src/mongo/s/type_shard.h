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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * This class represents the layout and contents of documents contained in the
     * config.shards collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(ShardType::name("shard0000"));
     *     shardDoc = conn->findOne(ShardType::ConfigNS, query);
     *
     *     // Process the response.
     *     ShardType shard;
     *     shard.fromBSON(shardDoc);
     *     if (! shard.isValid()) {
     *         // Can't use 'shard'. Take action.
     *     }
     *     // use 'shard'
     *
     */
    class ShardType {
        MONGO_DISALLOW_COPYING(ShardType);
    public:

        //
        // schema declarations
        //

        // Name of the shard collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the shard collection type.
        static BSONField<std::string> name;
        static BSONField<std::string> host;
        static BSONField<bool> draining;
        static BSONField<long long> maxSize;
        static BSONField<BSONArray> tags;

        //
        // shard type methods
        //

        ShardType();
        ~ShardType();

        /**
         * Returns true if all the mandatory fields are present and have valid
         * representations. Otherwise returns false and fills in the optional 'errMsg' string.
         */
        bool isValid(std::string* errMsg) const;

        /**
         * Returns the BSON representation of the entry.
         */
        BSONObj toBSON() const;

        /**
         * Clears and populates the internal state using the 'source' BSON object if the
         * latter contains valid values. Otherwise clear the internal state.
         */
        bool parseBSON(BSONObj source, std::string* errMsg);

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(ShardType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setName(const StringData& name) { _name = name.toString(); }
        const std::string& getName() const { return _name; }

        void setHost(const StringData& host) { _host = host.toString(); }
        const std::string& getHost() const { return _host; }

        void setDraining(bool draining) { _draining = draining; }
        bool getDraining() const { return _draining; }

        void setMaxSize(uint64_t maxSize) { _maxSize = maxSize; }
        uint64_t getMaxSize() const { return _maxSize; }

        void setTags(const BSONArray& tags) { _tags = tags; }
        BSONArray getTags() const { return _tags; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _name;   // (M) shard's id
        std::string _host;   // (M) connection string for the host(s)
        bool _draining;      // (O) is it draining chunks?
        long long _maxSize;  // (O) maximum allowed disk space in MB
        BSONArray _tags;     // (O) shard tags
    };

}  // namespace mongo
