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
#include "mongo/s/util.h" // for ShardChunkVersion
namespace mongo {

    /**
     * This class represents the layout and contents of documents contained in the
     * config.chunks collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     unique_ptr<DbClientCursor> cursor;
     *     BSONObj query = QUERY(ChunkType::ns("mydb.mycoll"));
     *     cursor.reset(conn->query(ChunkType::ConfigNS, query, ...));
     *
     *     // Process the response.
     *     while (cursor->more()) {
     *         chunkDoc = cursor->next();
     *         ChunkType chunk;
     *         chunk.fromBSON(dbDoc);
     *         if (! chunk.isValid()) {
     *             // Can't use 'chunk'. Take action.
     *         }
     *         // use 'chunk'
     *     }
     */
    class ChunkType {
        MONGO_DISALLOW_COPYING(ChunkType);
    public:

        //
        // schema declarations
        //

        // Name of the chunk collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the chunk collection type.
        static BSONField<std::string> name;
        static BSONField<std::string> ns;
        static BSONField<BSONObj> min;
        static BSONField<BSONObj> max;
        static BSONField<BSONArray> version;
        static BSONField<std::string> shard;
        static BSONField<bool> jumbo;

        // Transition to new format, 2.2 -> 2.4
        // 2.2 can read both lastmod + lastmodEpoch format and 2.4 [ lastmod, OID ] formats.
        static BSONField<Date_t> DEPRECATED_lastmod; // major | minor versions
        static BSONField<OID> DEPRECATED_epoch; // disambiguates collection incarnations

        //
        // chunk type methods
        //

        ChunkType();
        ~ChunkType();

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
        void cloneTo(ChunkType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setName(const StringData& name) {
            _name = name.toString();
        }

        const std::string& getName() const {
            return _name;
        }

        void setNS(const StringData& ns) {
            _ns = ns.toString();
        }

        const std::string& getNS() const {
            return _ns;
        }

        void setMin(const BSONObj& min) {
            _min = min.getOwned();
        }

        BSONObj getMin() const {
            return _min;
        }

        void setMax(const BSONObj& max) {
            _max = max.getOwned();
        }

        BSONObj getMax() const {
            return _max;
        }

        void setVersion(const ShardChunkVersion& version) {
            _version = version;
        }

        const ShardChunkVersion& getVersion() const {
            return _version;
        }

        void setShard(const StringData& shard) {
            _shard = shard.toString();
        }

        const std::string& getShard() const {
            return _shard;
        }

        void setJumbo(bool jumbo) {
            _jumbo = jumbo;
        }

        bool getJumbo() const {
            return _jumbo;
        }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        string _name; // (M) chunk's id
        string _ns; // (M) collection this chunk is in
        BSONObj _min; // (M) first key of the range, inclusive
        BSONObj _max; // (M) last key of the range, non-inclusive
        ShardChunkVersion _version; // (M) version of this chunk
        string _shard; // (M) shard this chunk lives in
        bool _jumbo; // (O) too big to move?
    };

} // namespace mongo
