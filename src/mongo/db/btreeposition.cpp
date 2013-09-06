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

#include "mongo/db/btreeposition.h"

#include "mongo/db/btree.h"
#include "mongo/db/index.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    std::ostream& operator<<( std::ostream& stream, const BtreeKeyLocation& loc ) {
        return stream << BSON( "bucket" << loc.bucket.toString() <<
                               "index" << loc.pos ).jsonString();
    }

    LogicalBtreePosition::LogicalBtreePosition( const IndexDetails& indexDetails,
                                                Ordering ordering,
                                                const BtreeKeyLocation& initialLocation ) :
        _indexDetails( &indexDetails ),
        _ordering( ordering ),
        _initialLocation( initialLocation ),
        _initialLocationValid() {
        fassert( 16494, _indexDetails->version() == 1 );
    }

    void LogicalBtreePosition::init() {
        if ( _initialLocation.bucket.isNull() ) {
            // Abort if the initial location is not a valid bucket.
            return;
        }

        // Store the key and record referenced at the supplied initial location.
        BucketBasics<V1>::KeyNode keyNode =
                _initialLocation.bucket.btree<V1>()->keyNode( _initialLocation.pos );
        _key = keyNode.key.toBson().getOwned();
        _record = keyNode.recordLoc;
        _initialLocationValid = true;
    }

    BtreeKeyLocation LogicalBtreePosition::currentLocation() const {
        if ( _initialLocation.bucket.isNull() ) {
            // Abort if the initial location is not a valid bucket.
            return BtreeKeyLocation();
        }

        // If the initial location has not been invalidated ...
        if ( _initialLocationValid ) {

            const BtreeBucket<V1>* bucket = _initialLocation.bucket.btree<V1>();
            if ( // ... and the bucket was not marked as invalid ...
                 bucket->getN() != bucket->INVALID_N_SENTINEL &&
                 // ... and the initial location index is valid for the bucket ...
                 _initialLocation.pos < bucket->getN() ) {

                BucketBasics<V1>::KeyNode keyNode = bucket->keyNode( _initialLocation.pos );
                if ( // ... and the record location equals the initial record location ...
                     keyNode.recordLoc == _record &&
                     // ... and the key equals the initial key ...
                     keyNode.key.toBson().binaryEqual( _key ) ) {
                    // ... then the initial location is the current location, so return it.
                    return _initialLocation;
                }
            }
        }

        // Relocate the key and record location retrieved from _initialLocation.
        BtreeKeyLocation ret;
        bool found;
        ret.bucket = _indexDetails->head.btree<V1>()->locate( *_indexDetails,
                                                              _indexDetails->head,
                                                              _key,
                                                              _ordering,
                                                              ret.pos,
                                                              found,
                                                              _record,
                                                              1 // Forward direction means the next
                                                                // ordered key will be returned if
                                                                // the requested key is missing.
                                                             );
        return ret;
    }

} // namespace mongo
