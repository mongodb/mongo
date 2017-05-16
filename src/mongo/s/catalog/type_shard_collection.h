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
 * config.collections collection. All manipulation of documents coming from that collection should
 * be done with this class.
 *
 * Expected shard server config.collections collection format:
 *   {
 *      "_id" : "foo.bar",                               // will become UUID when available
 *      "ns" : "foo.bar",
 *      "epoch" : ObjectId("58b6fd76132358839e409e47"),  // will remove when UUID becomes available
 *      "key" : {
 *          "_id" : 1
 *      },
 *      "defaultCollation" : {
 *          "locale" : "fr_CA"
 *      },
 *      "unique" : false,
 *      "refreshing" : true
 *      "refreshSequenceNumber" : 5
 *   }
 *
 */
class ShardCollectionType {
public:
    // Name of the collections collection on the shard server.
    static const std::string ConfigNS;

    static const BSONField<std::string> uuid;
    static const BSONField<std::string> ns;
    static const BSONField<OID> epoch;
    static const BSONField<BSONObj> keyPattern;
    static const BSONField<BSONObj> defaultCollation;
    static const BSONField<bool> unique;
    static const BSONField<bool> refreshing;
    static const BSONField<long long> refreshSequenceNumber;

    explicit ShardCollectionType(const NamespaceString& uuid,
                                 const NamespaceString& nss,
                                 const OID& epoch,
                                 const KeyPattern& keyPattern,
                                 const BSONObj& defaultCollation,
                                 const bool& unique);

    /**
     * Constructs a new ShardCollectionType object from BSON. Also does validation of the contents.
     */
    static StatusWith<ShardCollectionType> fromBSON(const BSONObj& source);

    /**
     * Returns the BSON representation of this shard collection type object.
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

    const NamespaceString& getNss() const {
        return _nss;
    }
    void setNss(const NamespaceString& nss);

    const OID& getEpoch() const {
        return _epoch;
    }
    void setEpoch(const OID& epoch);

    const KeyPattern& getKeyPattern() const {
        return _keyPattern;
    }
    void setKeyPattern(const KeyPattern& keyPattern);

    const BSONObj& getDefaultCollation() const {
        return _defaultCollation;
    }
    void setDefaultCollation(const BSONObj& collation) {
        _defaultCollation = collation.getOwned();
    }

    const bool getUnique() const {
        return _unique;
    }
    void setUnique(bool unique) {
        _unique = unique;
    }

    const bool hasRefreshing() const {
        return _refreshing.is_initialized();
    }
    const bool getRefreshing() const;
    void setRefreshing(bool refreshing) {
        _refreshing = refreshing;
    }

    const bool hasRefreshSequenceNumber() const {
        return _refreshSequenceNumber.is_initialized();
    }
    const long long getRefreshSequenceNumber() const;
    void setRefreshSequenceNumber(const long long seqNum) {
        _refreshSequenceNumber = seqNum;
    }

private:
    // Will become the UUID when available. Currently a duplicate of '_nss'.
    NamespaceString _uuid;

    // The full namespace (with the database prefix).
    NamespaceString _nss;

    // Uniquely identifies this instance of the collection, in case of drop/create.
    OID _epoch;

    // Sharding key. If collection is dropped, this is no longer required.
    KeyPattern _keyPattern;

    // Optional collection default collation. If empty, implies simple collation.
    BSONObj _defaultCollation;

    // Uniqueness of the sharding key.
    bool _unique;

    // Refresh fields set by primaries and used by shard secondaries to safely refresh chunk
    // metadata.
    boost::optional<bool> _refreshing{boost::none};
    boost::optional<long long> _refreshSequenceNumber{boost::none};
};

}  // namespace mongo
