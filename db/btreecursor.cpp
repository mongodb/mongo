// btreecursor.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "stdafx.h"
#include "btree.h"
#include "pdfile.h"
#include "jsobj.h"

namespace mongo {

    extern int otherTraceLevel;

    DiskLoc maxDiskLoc(0x7fffffff, 0x7fffffff);
    DiskLoc minDiskLoc(0, 1);

    BtreeCursor::BtreeCursor(IndexDetails& _id, const BSONObj& k, int _direction, BSONObj& _query) :
//    query(_query),
            indexDetails(_id),
            order(_id.keyPattern()),
            direction(_direction)
    {
//otherTraceLevel = 999;

        bool found;
        if ( otherTraceLevel >= 12 ) {
            if ( otherTraceLevel >= 200 ) {
                out() << "::BtreeCursor() qtl>200.  validating entire index." << endl;
                indexDetails.head.btree()->fullValidate(indexDetails.head, order);
            }
            else {
                out() << "BTreeCursor(). dumping head bucket" << endl;
                indexDetails.head.btree()->dump();
            }
        }

        findExtremeKeys( _query );
        if ( !k.isEmpty() )
            startKey = k.copy();

        bucket = indexDetails.head.btree()->
                 locate(indexDetails.head, startKey, order, keyOfs, found, direction > 0 ? minDiskLoc : maxDiskLoc, direction);

        checkUnused();
    }

// Given a query, find the lowest and highest keys along our index that could
// potentially match the query.  These lowest and highest keys will be mapped
// to startKey and endKey based on the value of direction.
    void BtreeCursor::findExtremeKeys( const BSONObj &query ) {
        BSONObjBuilder startBuilder;
        BSONObjBuilder endBuilder;
        vector< string >fields;
        getIndexFields( fields );
        for ( vector<string>::iterator i = fields.begin(); i != fields.end(); ++i ) {
            const char * field = i->c_str();
            BSONElement k = indexDetails.keyPattern().getFieldDotted( field );
            int number = (int) k.number(); // returns 0.0 if not numeric
            bool forward = ( ( number >= 0 ? 1 : -1 ) * direction > 0 );
            BSONElement lowest = minKey.firstElement();
            BSONElement highest = maxKey.firstElement();
            BSONElement e = query.getFieldDotted( field );
            BSONObjBuilder temp;
            if ( !e.eoo() && e.type() != RegEx ) {
                if ( getGtLtOp( e ) == JSMatcher::Equality )
                    lowest = highest = e;
                else
                    findExtremeInequalityValues( e, lowest, highest );
            } else if ( e.type() == RegEx ) {                                                      
                const char *r = e.simpleRegex();
                if ( r ) {                                                                         
                    temp.append( "", r );                                                          
                    lowest = temp.done().firstElement();                                           
                }
            }                  
            startBuilder.appendAs( forward ? lowest : highest, "" );
            endBuilder.appendAs( forward ? highest : lowest, "" );
        }
        startKey = startBuilder.doneAndDecouple();
        endKey = endBuilder.doneAndDecouple();
    }

// Find lowest and highest possible key values given all $gt, $gte, $lt, and
// $lte elements in e.  The values of lowest and highest should be
// preinitialized, for example to minKey.firstElement() and maxKey.firstElement().
    void BtreeCursor::findExtremeInequalityValues( const BSONElement &e,
            BSONElement &lowest,
            BSONElement &highest ) {
        BSONObjIterator i( e.embeddedObject() );
        while ( 1 ) {
            BSONElement s = i.next();
            if ( s.eoo() )
                break;
            int op = s.getGtLtOp();
            if ( ( op == JSMatcher::LT || op == JSMatcher::LTE ) &&
                    ( s.woCompare( highest, false ) < 0 ) )
                highest = s;
            else if ( ( op == JSMatcher::GT || op == JSMatcher::GTE ) &&
                      ( s.woCompare( lowest, false ) > 0 ) )
                lowest = s;
        }
    }

// Expand all field names in key to use dotted notation.
    void BtreeCursor::getFields( const BSONObj &key, vector< string > &fields ) {
        BSONObjIterator i( key );
        while ( 1 ) {
            BSONElement k = i.next();
            if ( k.eoo() )
                break;
            bool addedSubfield = false;
            if ( k.type() == Object ) {
                vector< string > subFields;
                getFields( k.embeddedObject(), subFields );
                for ( vector< string >::iterator i = subFields.begin(); i != subFields.end(); ++i ) {
                    addedSubfield = true;
                    fields.push_back( k.fieldName() + string( "." ) + *i );
                }
            }
            if ( !addedSubfield )
                fields.push_back( k.fieldName() );
        }
    }

    /* skip unused keys. */
    void BtreeCursor::checkUnused() {
        int u = 0;
        while ( 1 ) {
            if ( !ok() )
                break;
            BtreeBucket *b = bucket.btree();
            _KeyNode& kn = b->k(keyOfs);
            if ( kn.isUsed() )
                break;
            bucket = b->advance(bucket, keyOfs, direction, "checkUnused");
            u++;
        }
        if ( u > 10 )
            OCCASIONALLY log() << "btree unused skipped:" << u << '\n';
    }

// Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
    int sgn( int i ) {
        if ( i == 0 )
            return 0;
        return i > 0 ? 1 : -1;
    }

// Check if the current key is beyond endKey_.
    void BtreeCursor::checkEnd() {
        if ( bucket.isNull() )
            return;
        int cmp = sgn( endKey.woCompare( currKey(), order ) );
        if ( cmp != 0 && cmp != direction )
            bucket = DiskLoc();
    }

    bool BtreeCursor::advance() {
        if ( bucket.isNull() )
            return false;
        bucket = bucket.btree()->advance(bucket, keyOfs, direction, "BtreeCursor::advance");
        checkUnused();
        checkEnd();
        return !bucket.isNull();
    }

    void BtreeCursor::noteLocation() {
        if ( !eof() ) {
            BSONObj o = bucket.btree()->keyAt(keyOfs).copy();
            keyAtKeyOfs = o;
            locAtKeyOfs = bucket.btree()->k(keyOfs).recordLoc;
        }
    }

    /* Since the last noteLocation(), our key may have moved around, and that old cached
       information may thus be stale and wrong (although often it is right).  We check
       that here; if we have moved, we have to search back for where we were at.

       i.e., after operations on the index, the BtreeCursor's cached location info may
       be invalid.  This function ensures validity, so you should call it before using
       the cursor if other writers have used the database since the last noteLocation
       call.
    */
    void BtreeCursor::checkLocation() {
        if ( eof() )
            return;

        if ( keyOfs >= 0 ) {
            BtreeBucket *b = bucket.btree();

            assert( !keyAtKeyOfs.isEmpty() );

            // Note keyAt() returns an empty BSONObj if keyOfs is now out of range,
            // which is possible as keys may have been deleted.
            if ( b->keyAt(keyOfs).woEqual(keyAtKeyOfs) &&
                    b->k(keyOfs).recordLoc == locAtKeyOfs ) {
                if ( !b->k(keyOfs).isUsed() ) {
                    /* we were deleted but still exist as an unused
                       marker key. advance.
                    */
                    checkUnused();
                }
                return;
            }
        }

        /* normally we don't get to here.  when we do, old position is no longer
            valid and we must refind where we left off (which is expensive)
        */

        bool found;

        /* TODO: Switch to keep indexdetails and do idx.head! */
        bucket = indexDetails.head.btree()->locate(indexDetails.head, keyAtKeyOfs, order, keyOfs, found, locAtKeyOfs, direction);
        RARELY log() << "  key seems to have moved in the index, refinding. found:" << found << endl;
        if ( found )
            checkUnused();
    }

    /* ----------------------------------------------------------------------------- */

    struct BtreeUnitTest {
        BtreeUnitTest() {
            assert( minDiskLoc.compare(maxDiskLoc) < 0 );
        }
    } btut;

} // namespace mongo
