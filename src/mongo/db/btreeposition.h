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

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    class IndexDetails;

    /**
     * Physical location of a key within a btree.  Comprised of the DiskLoc address of a btree
     * bucket and the index of a key within that bucket.
     */
    struct BtreeKeyLocation {

        BtreeKeyLocation() :
            pos() {
        }

        BtreeKeyLocation( DiskLoc initialBucket, int32_t initialPos ) :
            bucket( initialBucket ),
            pos( initialPos ) {
        }

        bool operator==( const BtreeKeyLocation& other ) const {
            return bucket == other.bucket && pos == other.pos;
        }

        DiskLoc bucket; // Bucket within btree.
        int32_t pos;    // Index within bucket.
    };

    std::ostream& operator<<( std::ostream& stream, const BtreeKeyLocation& loc );

    /**
     * Logical btree position independent of the physical structure of a btree.  This is used to
     * track a position within a btree while the structure of the btree is changing.
     *
     * For example, a btree containing keys 'a', 'b', and 'c' might have all three keys in one
     * bucket or alternatively 'b' within a left child of 'c'.  The same LogicalBtreePosition can
     * represent the position of 'b' in both cases and can retrieve the physical BtreeKeyLocation of
     * 'b' in each case.  If the btree is changed so that it lacks a 'b' key, the position will
     * reference the lowest key greater than 'b'.  This is desirable behavior when the logical btree
     * position is used to implement a forward direction iterator.
     *
     * The class is seeded with a BtreeKeyLocation identifying a btree key.  This initial physical
     * location is cached in order to quickly check if the physical location corresponding to the
     * logical position is unchanged and can be returned as is.
     *
     * NOTE Only supports V1 indexes.
     */
    class LogicalBtreePosition {
    public:

        /**
         * Create a position with the @param 'indexDetails', @param 'ordering', and initial key
         * location @param 'initialLocation' specified.
         * @fasserts if 'indexDetails' is not a V1 index.
         */
        LogicalBtreePosition( const IndexDetails& indexDetails,
                              Ordering ordering,
                              const BtreeKeyLocation& initialLocation );

        /** Initialize the position by reading the key at the supplied initial location. */
        void init();

        /**
         * Invalidate the supplied initial location.  This may be called when bucket containing the
         * supplied location is deleted.
         */
        void invalidateInitialLocation() { _initialLocationValid = false; }

        /**
         * Retrieve the current physical location in the btree corresponding to this logical
         * position.
         */
        BtreeKeyLocation currentLocation() const;

    private:
        const IndexDetails* _indexDetails;
        Ordering _ordering;
        BtreeKeyLocation _initialLocation;
        bool _initialLocationValid;
        BSONObj _key;
        DiskLoc _record;
    };

} // namespace mongo
