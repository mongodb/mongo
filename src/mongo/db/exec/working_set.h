/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <vector>
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    struct WorkingSetMember;

    typedef long WorkingSetID;

    /**
     * All data in use by a query.  Data is passed through the stage tree by referencing the ID of
     * an element of the working set.  Stages can add elements to the working set, delete elements
     * from the working set, or mutate elements in the working set.
     */
    class WorkingSet {
    public:
        static const WorkingSetID INVALID_ID;

        WorkingSet();
        ~WorkingSet();

        /**
         * Allocate a new query result and return the ID used to get and free it.
         */
        WorkingSetID allocate();

        /**
         * Get the i-th mutable query result.
         */
        WorkingSetMember* get(const WorkingSetID& i);

        /**
         * Unallocate the i-th query result and release its resouces.
         */
        void free(const WorkingSetID& i);

    private:
        typedef unordered_map<WorkingSetID, WorkingSetMember*> DataMap;

        DataMap _data;

        // The WorkingSetID returned by the next call to allocate().  Should refer to the next valid
        // ID.  IDs allocated contiguously.  Should never point at an in-use ID.  
        WorkingSetID _nextId;
    };

    /**
     * The key data extracted from an index.  Keeps track of both the key (currently a BSONObj) and
     * the index that provided the key.  The index key pattern is required to correctly interpret
     * the key.
     */
    struct IndexKeyDatum {
        IndexKeyDatum(const BSONObj& keyPattern, const BSONObj& key) : indexKeyPattern(keyPattern),
                                                                       keyData(key) { }

        // This is not owned and points into the IndexDescriptor's data.
        BSONObj indexKeyPattern;

        // This is the BSONObj for the key that we put into the index.  Owned by us.
        BSONObj keyData;
    };

    /**
     * The type of the data passed between query stages.  In particular:
     *
     * Index scan stages return a WorkingSetMember in the LOC_AND_IDX state.
     *
     * Collection scan stages the LOC_AND_UNOWNED_OBJ state.
     *
     * A WorkingSetMember may have any of the data above.
     */
    struct WorkingSetMember {
        WorkingSetMember();

        enum MemberState {
            // Initial state.
            INVALID,

            // Data is from 1 or more indices.
            LOC_AND_IDX,

            // Data is from a collection scan.
            LOC_AND_UNOWNED_OBJ,

            // Data is from a fetch.
            LOC_AND_OWNED_OBJ,

            // DiskLoc has been invalidated, or the obj doesn't correspond to an on-disk document
            // anymore (e.g. is a computed expression).
            OWNED_OBJ,
        };

        DiskLoc loc;
        BSONObj obj;
        vector<IndexKeyDatum> keyData;
        MemberState state;

        bool hasLoc() const;
        bool hasObj() const;
        bool hasOwnedObj() const;
        bool hasUnownedObj() const;

        /**
         * getFieldDotted uses its state (obj or index data) to produce the field with the provided
         * name.
         *
         * Returns true if there is the element is in an index key or in an (owned or unowned)
         * object.  *out is set to the element if so.
         *
         * Returns false otherwise.  Returning false indicates a query planning error.
         */
        bool getFieldDotted(const string& field, BSONElement* out);
    };

}  // namespace mongo
