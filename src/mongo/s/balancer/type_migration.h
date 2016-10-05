/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/s/balancer/balancer_policy.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"

namespace mongo {

/**
 * This class represents the layout and contents of documents contained in the config.migrations
 * collection. All manipulation of documents coming from that collection should be done with this
 * class.
 */
class MigrationType {
public:
    // Name of the migrations collection in the config server.
    static const std::string ConfigNS;

    // Field names and types in the migrations collection type.
    static const BSONField<std::string> name;
    static const BSONField<std::string> ns;
    static const BSONField<BSONObj> min;
    static const BSONField<BSONObj> max;
    static const BSONField<std::string> fromShard;
    static const BSONField<std::string> toShard;
    static const BSONField<std::string> chunkVersionField;
    static const BSONField<std::string> collectionVersionField;

    /**
     * The Balancer encapsulates migration information in MigrateInfo objects, so this facilitates
     * conversion to a config.migrations entry format.
     */
    explicit MigrationType(MigrateInfo info);

    /**
     * Constructs a new MigrationType object from BSON. Expects all fields to be present, and errors
     * if they are not.
     */
    static StatusWith<MigrationType> fromBSON(const BSONObj& source);

    /**
     * Returns the BSON representation of the config.migrations document entry.
     */
    BSONObj toBSON() const;

    /**
     * Helper function for the Balancer that uses MigrateInfo objects to schedule migrations.
     */
    MigrateInfo toMigrateInfo() const;

    /**
     * Uniquely identifies a chunk by collection and min key.
     */
    std::string getName() const;

private:
    MigrationType();

    // Required fields for config.migrations.
    boost::optional<NamespaceString> _nss;
    boost::optional<BSONObj> _min;
    boost::optional<BSONObj> _max;
    boost::optional<ShardId> _fromShard;
    boost::optional<ShardId> _toShard;
};

}  // namespace mongo
