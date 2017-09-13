/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/uuid.h"

namespace mongo {

extern bool enableCollectionUUIDs;  // TODO(SERVER-27993) Replace based on upgrade/downgrade state.

/**
 * A CollectionUUID is a 128-bit unique identifier, per RFC 4122, v4. for a database collection.
 * Newly created collections are assigned a new randomly generated CollectionUUID. In a replica-set
 * or a sharded cluster, all nodes will use the same UUID for a given collection. The UUID stays
 * with the collection until it is dropped, so even across renames. A copied collection must have
 * its own new unique UUID though.
 */
using CollectionUUID = UUID;

using OptionalCollectionUUID = boost::optional<CollectionUUID>;

struct CollectionOptions {
    /**
     * Returns true if the options indicate the namespace is a view.
     */
    bool isView() const;

    /**
     * The 'uuid' member is a collection property stored in the catalog with user-settable options,
     * but is not valid for the user to specify as collection option. So, parsing commands must
     * reject the 'uuid' property, but parsing stored options must accept it.
     */
    enum ParseKind { parseForCommand, parseForStorage };

    /**
     * Confirms that collection options can be converted to BSON and back without errors.
     */
    Status validateForStorage() const;

    /**
     * Parses the "options" subfield of the collection info object.
     */
    Status parse(const BSONObj& obj, ParseKind kind = parseForCommand);

    BSONObj toBSON() const;

    /**
     * @param max in and out, will be adjusted
     * @return if the value is valid at all
     */
    static bool validMaxCappedDocs(long long* max);

    // ----

    // Collection UUID. Will exist if featureCompatibilityVersion >= 3.6.
    OptionalCollectionUUID uuid;

    bool capped = false;
    long long cappedSize = 0;
    long long cappedMaxDocs = 0;

    // (MMAPv1) The following 2 are mutually exclusive, can only have one set.
    long long initialNumExtents = 0;
    std::vector<long long> initialExtentSizes;

    // The behavior of _id index creation when collection created
    void setNoIdIndex() {
        autoIndexId = NO;
    }
    enum {
        DEFAULT,  // currently yes for most collections, NO for some system ones
        YES,      // create _id index
        NO        // do not create _id index
    } autoIndexId = DEFAULT;

    // user flags
    enum UserFlags {
        Flag_UsePowerOf2Sizes = 1 << 0,
        Flag_NoPadding = 1 << 1,
    };
    int flags = Flag_UsePowerOf2Sizes;  // a bitvector of UserFlags
    bool flagsSet = false;

    bool temp = false;

    // Storage engine collection options. Always owned or empty.
    BSONObj storageEngine;

    // Default options for indexes created on the collection. Always owned or empty.
    BSONObj indexOptionDefaults;

    // Always owned or empty.
    BSONObj validator;
    std::string validationAction;
    std::string validationLevel;

    // The namespace's default collation.
    BSONObj collation;

    // View-related options.
    // The namespace of the view or collection that "backs" this view, or the empty string if this
    // collection is not a view.
    std::string viewOn;
    // The aggregation pipeline that defines this view.
    BSONObj pipeline;
};
}
