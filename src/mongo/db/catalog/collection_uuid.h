/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include <string>

#include "mongo/db/jsobj.h"

namespace mongo {

extern bool enableCollectionUUIDs;  // TODO(SERVER-27993) Replace based on upgrade/downgrade state.

/**
 * A CollectionUUID is a 128-bit unique identifier, per RFC 4122, v4. for a database collection.
 * Newly created collections are assigned a new randomly generated CollectionUUID. In a replica-set
 * or a sharded cluster, all nodes will use the same UUID for a given collection. The UUID stays
 * with the collection until it is dropped, so even across renames. A copied collection must have
 * own new new unique UUID though.
 */
class CollectionUUID {
public:
    using UUID = std::array<unsigned char, 16>;
    CollectionUUID() = delete;
    CollectionUUID(const CollectionUUID& other) = default;

    inline bool operator==(const CollectionUUID& rhs) const {
        return !memcmp(&_uuid, &rhs._uuid, sizeof(_uuid));
    }

    inline bool operator!=(const CollectionUUID& rhs) const {
        return !(*this == rhs);
    }

    /**
     * Parse a UUID from the given element. Caller must validate the input.
     */
    CollectionUUID(BSONElement from) : _uuid(from.uuid()) {}

    /**
     * Generate a new random v4 UUID per RFC 4122.
     */
    static CollectionUUID generateSecureRandomUUID();

    /**
     * Return a BSON object of the form { uuid: BinData(4, "...") }.
     */
    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.appendBinData("uuid", sizeof(UUID), BinDataType::newUUID, &_uuid);
        return builder.obj();
    }

private:
    CollectionUUID(const UUID& uuid) : _uuid(uuid) {}
    UUID _uuid;  // UUID in network byte order
};
}
