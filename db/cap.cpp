// @file cap.cpp capped collection related 
// the "old" version (<= v1.6)

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

#include "pch.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "../scripting/engine.h"
#include "btree.h"
#include <algorithm>
#include <list>
#include "query.h"
#include "queryutil.h"
#include "json.h"

/*
 capped collection layout

 d's below won't exist if things align perfectly:

 extent1             -> extent2                 -> extent3
 -------------------    -----------------------    ---------------------
 d r r r r r r r r d    d r r r r d r r r r r d    d r r r r r r r r r d
                                ^   ^
                           oldest   newest

                        ^cappedFirstDeletedInCurExtent()
                   ^cappedLastDelRecLastExtent()
 ^cappedListOfAllDeletedRecords()
*/


namespace mongo {

    /* combine adjacent deleted records *for the current extent* of the capped collection
     
       this is O(n^2) but we call it for capped tables where typically n==1 or 2!
       (or 3...there will be a little unused sliver at the end of the extent.)
    */
    void NamespaceDetails::compact() {
        assert(capped);

        list<DiskLoc> drecs;

        // Pull out capExtent's DRs from deletedList
        DiskLoc i = cappedFirstDeletedInCurExtent();
        for (; !i.isNull() && inCapExtent( i ); i = i.drec()->nextDeleted )
            drecs.push_back( i );
        cappedFirstDeletedInCurExtent() = i;

        // This is the O(n^2) part.
        drecs.sort();

        list<DiskLoc>::iterator j = drecs.begin();
        assert( j != drecs.end() );
        DiskLoc a = *j;
        while ( 1 ) {
            j++;
            if ( j == drecs.end() ) {
                DEBUGGING out() << "TEMP: compact adddelrec\n";
                addDeletedRec(a.drec(), a);
                break;
            }
            DiskLoc b = *j;
            while ( a.a() == b.a() && a.getOfs() + a.drec()->lengthWithHeaders == b.getOfs() ) {
                // a & b are adjacent.  merge.
                a.drec()->lengthWithHeaders += b.drec()->lengthWithHeaders;
                j++;
                if ( j == drecs.end() ) {
                    DEBUGGING out() << "temp: compact adddelrec2\n";
                    addDeletedRec(a.drec(), a);
                    return;
                }
                b = *j;
            }
            DEBUGGING out() << "temp: compact adddelrec3\n";
            addDeletedRec(a.drec(), a);
            a = b;
        }
    }

    DiskLoc &NamespaceDetails::cappedFirstDeletedInCurExtent() {
        if ( cappedLastDelRecLastExtent().isNull() )
            return cappedListOfAllDeletedRecords();
        else
            return cappedLastDelRecLastExtent().drec()->nextDeleted;
    }

    void NamespaceDetails::cappedCheckMigrate() {
        // migrate old NamespaceDetails format
        assert( capped );
        if ( capExtent.a() == 0 && capExtent.getOfs() == 0 ) {
            capFirstNewRecord = DiskLoc();
            capFirstNewRecord.setInvalid();
            // put all the DeletedRecords in cappedListOfAllDeletedRecords()
            for ( int i = 1; i < Buckets; ++i ) {
                DiskLoc first = deletedList[ i ];
                if ( first.isNull() )
                    continue;
                DiskLoc last = first;
                for (; !last.drec()->nextDeleted.isNull(); last = last.drec()->nextDeleted );
                last.drec()->nextDeleted = cappedListOfAllDeletedRecords();
                cappedListOfAllDeletedRecords() = first;
                deletedList[ i ] = DiskLoc();
            }
            // NOTE cappedLastDelRecLastExtent() set to DiskLoc() in above

            // Last, in case we're killed before getting here
            capExtent = firstExtent;
        }
    }

    bool NamespaceDetails::inCapExtent( const DiskLoc &dl ) const {
        assert( !dl.isNull() );
        // We could have a rec or drec, doesn't matter.
        return dl.drec()->myExtent( dl ) == capExtent.ext();
    }

    bool NamespaceDetails::nextIsInCapExtent( const DiskLoc &dl ) const {
        assert( !dl.isNull() );
        DiskLoc next = dl.drec()->nextDeleted;
        if ( next.isNull() )
            return false;
        return inCapExtent( next );
    }

    void NamespaceDetails::advanceCapExtent( const char *ns ) {
        // We want cappedLastDelRecLastExtent() to be the last DeletedRecord of the prev cap extent
        // (or DiskLoc() if new capExtent == firstExtent)
        if ( capExtent == lastExtent )
            cappedLastDelRecLastExtent() = DiskLoc();
        else {
            DiskLoc i = cappedFirstDeletedInCurExtent();
            for (; !i.isNull() && nextIsInCapExtent( i ); i = i.drec()->nextDeleted );
            cappedLastDelRecLastExtent() = i;
        }

        capExtent = theCapExtent()->xnext.isNull() ? firstExtent : theCapExtent()->xnext;

        /* this isn't true if a collection has been renamed...that is ok just used for diagnostics */
        //dassert( theCapExtent()->ns == ns );

        theCapExtent()->assertOk();
        capFirstNewRecord = DiskLoc();
    }

    DiskLoc NamespaceDetails::__capAlloc( int len ) {
        DiskLoc prev = cappedLastDelRecLastExtent();
        DiskLoc i = cappedFirstDeletedInCurExtent();
        DiskLoc ret;
        for (; !i.isNull() && inCapExtent( i ); prev = i, i = i.drec()->nextDeleted ) {
            // We need to keep at least one DR per extent in cappedListOfAllDeletedRecords(),
            // so make sure there's space to create a DR at the end.
            if ( i.drec()->lengthWithHeaders >= len + 24 ) {
                ret = i;
                break;
            }
        }

        /* unlink ourself from the deleted list */
        if ( !ret.isNull() ) {
            if ( prev.isNull() )
                cappedListOfAllDeletedRecords() = ret.drec()->nextDeleted;
            else
                prev.drec()->nextDeleted = ret.drec()->nextDeleted;
            ret.drec()->nextDeleted.setInvalid(); // defensive.
            assert( ret.drec()->extentOfs < ret.getOfs() );
        }

        return ret;
    }

    DiskLoc NamespaceDetails::cappedAlloc(const char *ns, int len) { 
        // signal done allocating new extents.
        if ( !cappedLastDelRecLastExtent().isValid() )
            cappedLastDelRecLastExtent() = DiskLoc();
        
        assert( len < 400000000 );
        int passes = 0;
        int maxPasses = ( len / 30 ) + 2; // 30 is about the smallest entry that could go in the oplog
        if ( maxPasses < 5000 ){
            // this is for bacwards safety since 5000 was the old value
            maxPasses = 5000;
        }
        DiskLoc loc;

        // delete records until we have room and the max # objects limit achieved.

        /* this fails on a rename -- that is ok but must keep commented out */
        //assert( theCapExtent()->ns == ns );

        theCapExtent()->assertOk();
        DiskLoc firstEmptyExtent;
        while ( 1 ) {
            if ( nrecords < max ) {
                loc = __capAlloc( len );
                if ( !loc.isNull() )
                    break;
            }

            // If on first iteration through extents, don't delete anything.
            if ( !capFirstNewRecord.isValid() ) {
                advanceCapExtent( ns );
                if ( capExtent != firstExtent )
                    capFirstNewRecord.setInvalid();
                // else signal done with first iteration through extents.
                continue;
            }

            if ( !capFirstNewRecord.isNull() &&
                    theCapExtent()->firstRecord == capFirstNewRecord ) {
                // We've deleted all records that were allocated on the previous
                // iteration through this extent.
                advanceCapExtent( ns );
                continue;
            }

            if ( theCapExtent()->firstRecord.isNull() ) {
                if ( firstEmptyExtent.isNull() )
                    firstEmptyExtent = capExtent;
                advanceCapExtent( ns );
                if ( firstEmptyExtent == capExtent ) {
                    maybeComplain( ns, len );
                    return DiskLoc();
                }
                continue;
            }

            DiskLoc fr = theCapExtent()->firstRecord;
            theDataFileMgr.deleteRecord(ns, fr.rec(), fr, true); // ZZZZZZZZZZZZ
            compact();
            if( ++passes > maxPasses ) {
                log() << "passes ns:" << ns << " len:" << len << " maxPasses: " << maxPasses << '\n';
                log() << "passes max:" << max << " nrecords:" << nrecords << " datasize: " << datasize << endl;
                massert( 10345 ,  "passes >= maxPasses in capped collection alloc", false );
            }
        }

        // Remember first record allocated on this iteration through capExtent.
        if ( capFirstNewRecord.isValid() && capFirstNewRecord.isNull() )
            capFirstNewRecord = loc;

        return loc;
    }

    void NamespaceDetails::dumpExtents() {
        cout << "dumpExtents:" << endl;
        for ( DiskLoc i = firstExtent; !i.isNull(); i = i.ext()->xnext ) {
            Extent *e = i.ext();
            stringstream ss;
            e->dump(ss);
            cout << ss.str() << endl;
        }
    }

    void NamespaceDetails::cappedDumpDelInfo() { 
        cout << "dl[0]: " << deletedList[0].toString() << endl;
        for( DiskLoc z = deletedList[0]; !z.isNull(); z = z.drec()->nextDeleted ) { 
            cout << "  drec:" << z.toString() << " dreclen:" << hex << z.drec()->lengthWithHeaders << 
                " ext:" << z.drec()->myExtent(z)->myLoc.toString() << endl;
        }
        cout << "dl[1]: " << deletedList[1].toString() << endl;
    }

    /* everything from end on, eliminate from the capped collection.
       @param inclusive if true, deletes end (i.e. closed or open range)
    */
    void NamespaceDetails::cappedTruncateAfter(const char *ns, DiskLoc end, bool inclusive) {
        DEV assert( this == nsdetails(ns) );
        assert( cappedLastDelRecLastExtent().isValid() );
        
        bool foundLast = false;
        while( 1 ) {
            if ( foundLast ) {
                break;
            }
            DiskLoc curr = theCapExtent()->lastRecord;
            assert( !curr.isNull() );
            if ( curr == end ) {
                if ( inclusive ) {
                    foundLast = true;
                } else {
                    break;
                }
            }
            
            uassert( 13415, "emptying the collection is not allowed", nrecords > 1 );
            
            if ( !capLooped() ) {
                theDataFileMgr.deleteRecord(ns, curr.rec(), curr, true);
                compact();
                if ( theCapExtent()->lastRecord.isNull() ) {
                    assert( !theCapExtent()->xprev.isNull() );
                    capExtent = theCapExtent()->xprev;
                    theCapExtent()->assertOk();
                    if ( capExtent == firstExtent ) {
                        cappedLastDelRecLastExtent() = DiskLoc();
                    } else {
                        // slow - there's no prev ptr for deleted rec
                        DiskLoc i = cappedListOfAllDeletedRecords();
                        for( ;
                            !i.drec()->nextDeleted.isNull() &&
                            !inCapExtent( i.drec()->nextDeleted );
                            i = i.drec()->nextDeleted );
                        assert( !i.drec()->nextDeleted.isNull() ); // I believe there is always at least one drec per extent
                        cappedLastDelRecLastExtent() = i;
                    }
                }
                continue;
            }

            theDataFileMgr.deleteRecord(ns, curr.rec(), curr, true);
            compact();
            if ( curr == capFirstNewRecord ) { // invalid, but can compare locations
                capExtent = ( capExtent == firstExtent ) ? lastExtent : theCapExtent()->xprev;
                theCapExtent()->assertOk();
                assert( !theCapExtent()->firstRecord.isNull() );
                capFirstNewRecord = theCapExtent()->firstRecord;
                if ( capExtent == firstExtent ) {
                    cappedLastDelRecLastExtent() = DiskLoc();
                } else {
                    // slow - there's no prev ptr for deleted rec
                    DiskLoc i = cappedListOfAllDeletedRecords();
                    for( ;
                        !i.drec()->nextDeleted.isNull() &&
                        !inCapExtent( i.drec()->nextDeleted );
                        i = i.drec()->nextDeleted );
                    assert( !i.drec()->nextDeleted.isNull() ); // I believe there is always at least one drec per extent
                    cappedLastDelRecLastExtent() = i;
                }
            }
        }
    }
    
    void NamespaceDetails::emptyCappedCollection( const char *ns ) {
        DEV assert( this == nsdetails(ns) );
        massert( 13424, "collection must be capped", capped );
        massert( 13425, "background index build in progress", !backgroundIndexBuildInProgress );
        massert( 13426, "indexes present", nIndexes == 0 );

        ClientCursor::invalidate( ns );
		NamespaceDetailsTransient::clearForPrefix( ns );

        cappedLastDelRecLastExtent() = DiskLoc();
        cappedListOfAllDeletedRecords() = DiskLoc();
        
        // preserve firstExtent/lastExtent
        capExtent = firstExtent;
        datasize = nrecords = 0;
        // lastExtentSize preserve
        // nIndexes preserve 0
        // capped preserve true
        // max preserve
        paddingFactor = 1.0;
        flags = 0;
        capFirstNewRecord = DiskLoc();
        capFirstNewRecord.setInvalid();
        cappedLastDelRecLastExtent().setInvalid();
        // dataFileVersion preserve
        // indexFileVersion preserve
        multiKeyIndexBits = 0;
        reservedA = 0;
        extraOffset = 0;
        // backgroundIndexBuildInProgress preserve 0
        memset(reserved, 0, sizeof(reserved));

        for( DiskLoc ext = firstExtent; !ext.isNull(); ext = ext.ext()->xnext ) {
            DiskLoc prev = ext.ext()->xprev;
            DiskLoc next = ext.ext()->xnext;
            DiskLoc empty = ext.ext()->reuse( ns );
            ext.ext()->xprev = prev;
            ext.ext()->xnext = next;
            addDeletedRec( empty.drec(), empty );
        }
    }

}
