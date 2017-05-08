/**
 *    Copyright (C) 2012-2015 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class BSONObjBuilder;
class Status;
template <typename T>
class StatusWith;

/**
 * Contains the minimum representation of a chunk - its bounds in the format [min, max) along with
 * utilities for parsing and persistence.
 */
class ChunkRange {
public:
    ChunkRange(BSONObj minKey, BSONObj maxKey);

    /**
     * Parses a chunk range using the format { min: <min bound>, max: <max bound> }.
     */
    static StatusWith<ChunkRange> fromBSON(const BSONObj& obj);

    const BSONObj& getMin() const {
        return _minKey;
    }

    const BSONObj& getMax() const {
        return _maxKey;
    }

    /**
     * Checks whether the specified key is within the bounds of this chunk range.
     */
    bool containsKey(const BSONObj& key) const;

    /**
     * Writes the contents of this chunk range as { min: <min bound>, max: <max bound> }.
     */
    void append(BSONObjBuilder* builder) const;

    std::string toString() const;

    /**
     * Returns true if two chunk ranges match exactly in terms of the min and max keys (including
     * element order within the keys).
     */
    bool operator==(const ChunkRange& other) const;
    bool operator!=(const ChunkRange& other) const;

    /**
     * Returns true iff the union of *this and the argument range is the same as *this.
     */
    bool covers(ChunkRange const& other) const;

    /**
     * Returns the range of overlap between *this and other, if any.
     */
    boost::optional<ChunkRange> overlapWith(ChunkRange const& other) const;

    /**
     * Returns a range that includes *this and other. If the ranges do not overlap, it includes
     * all the space between, as well.
     */
    ChunkRange unionWith(ChunkRange const& other) const;

private:
    const BSONObj _minKey;
    const BSONObj _maxKey;
};

/**
 * This class represents the layouts and contents of documents contained in the config server's
 * config.chunks and shard server's config.chunks.uuid collections. All manipulation of documents
 * coming from these collections should be done with this class. The shard's config.chunks.uuid
 * collections use the epoch field as the uuid value, and epochs match 1:1 to collection instances
 * (mmapped in config.collections). Therefore, the shard collections do not need to include epoch or
 * namespace fields, as these will be known in order to access the collections.
 *
 * Expected config server config.chunks collection format:
 *   {
 *      _id : "test.foo-a_MinKey",
 *      ns : "test.foo",
 *      min : {
 *              "a" : { "$minKey" : 1 }
 *      },
 *      max : {
 *              "a" : { "$maxKey" : 1 }
 *      },
 *      shard : "test-rs1",
 *      lastmod : Timestamp(1, 0),
 *      lastmodEpoch : ObjectId("587fc60cef168288439ad6ed"),
 *      jumbo : false              // optional field
 *   }
 *
 * Expected shard server config.chunks.<epoch> collection format:
 *   {
 *      _id: {
 *             "a" : { "$minKey" : 1 }
 *      }
 *      max : {
 *              "a" : { "$maxKey" : 1 }
 *      }
 *      shard : "test-rs1",
 *      lastmod : Timestamp(1, 0),
 *   }
 *
 * Note: it is intended to change the config server's collection schema to mirror the new shard
 * server's collection schema, but that will be future work when the new schema is stable and there
 * is time to do the extra work, as well as handle the backwards compatibility issues it poses.
 */
class ChunkType {
public:
    // Name of the chunks collection in the config server.
    static const std::string ConfigNS;

    // The shard chunks collections' common namespace prefix.
    static const std::string ShardNSPrefix;

    // Field names and types in the chunks collections.
    static const BSONField<std::string> name;
    static const BSONField<BSONObj> minShardID;
    static const BSONField<std::string> ns;
    static const BSONField<BSONObj> min;
    static const BSONField<BSONObj> max;
    static const BSONField<std::string> shard;
    static const BSONField<bool> jumbo;
    static const BSONField<Date_t> DEPRECATED_lastmod;
    static const BSONField<OID> DEPRECATED_epoch;

    ChunkType();
    ChunkType(NamespaceString nss, ChunkRange range, ChunkVersion version, ShardId shardId);

    /**
     * Constructs a new ChunkType object from BSON that has the config server's config.chunks
     * collection format.
     *
     * Also does validation of the contents.
     */
    static StatusWith<ChunkType> fromConfigBSON(const BSONObj& source);

    /**
     * Returns the BSON representation of the entry for the config server's config.chunks
     * collection.
     */
    BSONObj toConfigBSON() const;

    /**
     * Constructs a new ChunkType object from BSON that has a shard server's config.chunks.<epoch>
     * collection format.
     *
     * Also does validation of the contents.
     */
    static StatusWith<ChunkType> fromShardBSON(const BSONObj& source, const OID& epoch);

    /**
     * Returns the BSON representation of the entry for a shard server's config.chunks.<epoch>
     * collection.
     */
    BSONObj toShardBSON() const;

    std::string getName() const;

    /**
     * Getters and setters.
     */
    const std::string& getNS() const {
        return _ns.get();
    }
    void setNS(const std::string& name);

    const BSONObj& getMin() const {
        return _min.get();
    }
    void setMin(const BSONObj& min);

    const BSONObj& getMax() const {
        return _max.get();
    }
    void setMax(const BSONObj& max);

    ChunkRange getRange() const {
        return ChunkRange(getMin(), getMax());
    }

    bool isVersionSet() const {
        return _version.is_initialized();
    }
    const ChunkVersion& getVersion() const {
        return _version.get();
    }
    void setVersion(const ChunkVersion& version);

    const ShardId& getShard() const {
        return _shard.get();
    }
    void setShard(const ShardId& shard);

    bool getJumbo() const {
        return _jumbo.get_value_or(false);
    }
    void setJumbo(bool jumbo);

    /**
     * Generates chunk id based on the namespace name and the lower bound of the chunk.
     */
    static std::string genID(StringData ns, const BSONObj& min);

    /**
     * Returns OK if all the mandatory fields have been set. Otherwise returns NoSuchKey and
     * information about the first field that is missing.
     */
    Status validate() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial; (C)onfig, (S)hard.

    // (O)(C)     collection this chunk is in
    boost::optional<std::string> _ns;
    // (M)(C)(S)  first key of the range, inclusive
    boost::optional<BSONObj> _min;
    // (M)(C)(S)  last key of the range, non-inclusive
    boost::optional<BSONObj> _max;
    // (M)(C)(S)  version of this chunk
    boost::optional<ChunkVersion> _version;
    // (M)(C)(S)  shard this chunk lives in
    boost::optional<ShardId> _shard;
    // (O)(C)     too big to move?
    boost::optional<bool> _jumbo;
};

}  // namespace mongo
