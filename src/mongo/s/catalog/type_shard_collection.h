/**
 *    Copyright (C) 2017 10gen Inc.
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

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

class CollectionType;
class Status;
template <typename T>
class StatusWith;

/**
 * This class represents the layout and contents of documents contained in the shard server's
 * config.collections collections. All manipulation of documents coming from that collection should
 * be done with this class.
 *
 * Expected shard server config.collections collection format:
 *   {
 *      "_id" : "foo.bar",  // will eventually become a UUID field, when it becomes available
 *      "ns" : "foo.bar",
 *      "key" : {
 *          "_id" : 1
 *      },
 *      "lastConsistentCollectionVersionEpoch" : ObjectId("58b6fd76132358839e409e47"),
 *      "lastConsistentCollectionVersion" : ISODate("1970-02-19T17:02:47.296Z")
 *   }
 *
 * The 'lastConsistentCollectionVersion' is written by shard primaries and used by shard
 * secondaries. A secondary uses the value to refresh chunk metadata up to the chunk with that
 * chunk version. Chunk metadata updates on the shard involve multiple chunks collection document
 * writes, during which time the data can be inconsistent and should not be loaded.
 */
class ShardCollectionType {
public:
    // Name of the collections collection in the config server.
    static const std::string ConfigNS;

    static const BSONField<std::string> uuid;
    static const BSONField<std::string> ns;
    static const BSONField<BSONObj> keyPattern;
    static const BSONField<OID> lastConsistentCollectionVersionEpoch;
    static const BSONField<Date_t> lastConsistentCollectionVersion;

    explicit ShardCollectionType(const CollectionType& collType);

    /**
     * Constructs a new ShardCollectionType object from BSON retrieved from a shard server. Also
     * does validation of the contents.
     */
    static StatusWith<ShardCollectionType> fromBSON(const BSONObj& source);

    /**
     * Returns the BSON representation of the entry for the shard collection schema.
     *
     * This function only appends the fields and values relevant to shards that are SET on the
     * ShardCollectionType object. No field is guaranteed to be appended.
     */
    BSONObj toBSON() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const NamespaceString& getUUID() const {
        return _uuid;
    }
    void setUUID(const NamespaceString& uuid);

    const NamespaceString& getNs() const {
        return _ns;
    }
    void setNs(const NamespaceString& ns);

    const KeyPattern& getKeyPattern() const {
        return _keyPattern;
    }
    void setKeyPattern(const KeyPattern& keyPattern);

    const ChunkVersion& getLastConsistentCollectionVersion() const {
        return _lastConsistentCollectionVersion.get();
    }
    void setLastConsistentCollectionVersion(const ChunkVersion& lastConsistentCollectionVersion);

    bool isLastConsistentCollectionVersionSet() const;

    const OID getLastConsistentCollectionVersionEpoch() const;

private:
    ShardCollectionType(const NamespaceString& uuid,
                        const NamespaceString& ns,
                        const KeyPattern& keyPattern);

    NamespaceString _uuid;

    // The full namespace (with the database prefix).
    NamespaceString _ns;

    // Sharding key. If collection is dropped, this is no longer required.
    KeyPattern _keyPattern;

    // used by shard secondaries to safely refresh chunk metadata up to this version: higher
    // versions may put the chunk metadata into an inconsistent state.
    boost::optional<ChunkVersion> _lastConsistentCollectionVersion;
};

}  // namespace mongo
