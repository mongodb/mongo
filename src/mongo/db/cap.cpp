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
#include "json.h"
#include "clientcursor.h"

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
        verify( isCapped() );

        list<DiskLoc> drecs;

        // Pull out capExtent's DRs from deletedList
        DiskLoc i = cappedFirstDeletedInCurExtent();
        for (; !i.isNull() && inCapExtent( i ); i = i.drec()->nextDeleted() )
            drecs.push_back( i );

        getDur().writingDiskLoc( cappedFirstDeletedInCurExtent() ) = i;

        // This is the O(n^2) part.
        drecs.sort();

        list<DiskLoc>::iterator j = drecs.begin();
        verify( j != drecs.end() );
        DiskLoc a = *j;
        while ( 1 ) {
            j++;
            if ( j == drecs.end() ) {
                DEBUGGING out() << "TEMP: compact adddelrec\n";
                addDeletedRec(a.drec(), a);
                break;
            }
            DiskLoc b = *j;
            while ( a.a() == b.a() && a.getOfs() + a.drec()->lengthWithHeaders() == b.getOfs() ) {
                // a & b are adjacent.  merge.
                getDur().writingInt( a.drec()->lengthWithHeaders() ) += b.drec()->lengthWithHeaders();
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
            return cappedLastDelRecLastExtent().drec()->nextDeleted();
    }

    void NamespaceDetails::cappedCheckMigrate() {
        // migrate old NamespaceDetails format
        verify( isCapped() );
        if ( capExtent.a() == 0 && capExtent.getOfs() == 0 ) {
            //capFirstNewRecord = DiskLoc();
            capFirstNewRecord.writing().setInvalid();
            // put all the DeletedRecords in cappedListOfAllDeletedRecords()
            for ( int i = 1; i < Buckets; ++i ) {
                DiskLoc first = deletedList[ i ];
                if ( first.isNull() )
                    continue;
                DiskLoc last = first;
                for (; !last.drec()->nextDeleted().isNull(); last = last.drec()->nextDeleted() );
                last.drec()->nextDeleted().writing() = cappedListOfAllDeletedRecords();
                cappedListOfAllDeletedRecords().writing() = first;
                deletedList[i].writing() = DiskLoc();
            }
            // NOTE cappedLastDelRecLastExtent() set to DiskLoc() in above

            // Last, in case we're killed before getting here
            capExtent.writing() = firstExtent;
        }
    }

    bool NamespaceDetails::inCapExtent( const DiskLoc &dl ) const {
        verify( !dl.isNull() );
        // We could have a rec or drec, doesn't matter.
        bool res = dl.drec()->myExtentLoc(dl) == capExtent;
        DEV {
            // old implementation. this check is temp to test works the same.  new impl should be a little faster.
            verify( res == (dl.drec()->myExtent( dl ) == capExtent.ext()) );
        }
        return res;
    }

    bool NamespaceDetails::nextIsInCapExtent( const DiskLoc &dl ) const {
        verify( !dl.isNull() );
        DiskLoc next = dl.drec()->nextDeleted();
        if ( next.isNull() )
            return false;
        return inCapExtent( next );
    }

    void NamespaceDetails::advanceCapExtent( const char *ns ) {
        // We want cappedLastDelRecLastExtent() to be the last DeletedRecord of the prev cap extent
        // (or DiskLoc() if new capExtent == firstExtent)
        if ( capExtent == lastExtent )
            getDur().writingDiskLoc( cappedLastDelRecLastExtent() ) = DiskLoc();
        else {
            DiskLoc i = cappedFirstDeletedInCurExtent();
            for (; !i.isNull() && nextIsInCapExtent( i ); i = i.drec()->nextDeleted() );
            getDur().writingDiskLoc( cappedLastDelRecLastExtent() ) = i;
        }

        getDur().writingDiskLoc( capExtent ) = theCapExtent()->xnext.isNull() ? firstExtent : theCapExtent()->xnext;

        /* this isn't true if a collection has been renamed...that is ok just used for diagnostics */
        //dassert( theCapExtent()->ns == ns );

        theCapExtent()->assertOk();
        getDur().writingDiskLoc( capFirstNewRecord ) = DiskLoc();
    }

    DiskLoc NamespaceDetails::__capAlloc( int len ) {
        DiskLoc prev = cappedLastDelRecLastExtent();
        DiskLoc i = cappedFirstDeletedInCurExtent();
        DiskLoc ret;
        for (; !i.isNull() && inCapExtent( i ); prev = i, i = i.drec()->nextDeleted() ) {
            // We need to keep at least one DR per extent in cappedListOfAllDeletedRecords(),
            // so make sure there's space to create a DR at the end.
            if ( i.drec()->lengthWithHeaders() >= len + 24 ) {
                ret = i;
                break;
            }
        }

        /* unlink ourself from the deleted list */
        if ( !ret.isNull() ) {
            if ( prev.isNull() )
                cappedListOfAllDeletedRecords().writing() = ret.drec()->nextDeleted();
            else
                prev.drec()->nextDeleted().writing() = ret.drec()->nextDeleted();
            ret.drec()->nextDeleted().writing().setInvalid(); // defensive.
            verify( ret.drec()->extentOfs() < ret.getOfs() );
        }

        return ret;
    }

    DiskLoc NamespaceDetails::cappedAlloc(const char *ns, int len) {
        // signal done allocating new extents.
        if ( !cappedLastDelRecLastExtent().isValid() )
            getDur().writingDiskLoc( cappedLastDelRecLastExtent() ) = DiskLoc();

        verify( len < 400000000 );
        int passes = 0;
        int maxPasses = ( len / 30 ) + 2; // 30 is about the smallest entry that could go in the oplog
        if ( maxPasses < 5000 ) {
            // this is for bacwards safety since 5000 was the old value
            maxPasses = 5000;
        }
        DiskLoc loc;

        // delete records until we have room and the max # objects limit achieved.

        /* this fails on a rename -- that is ok but must keep commented out */
        //verify( theCapExtent()->ns == ns );

        theCapExtent()->assertOk();
        DiskLoc firstEmptyExtent;
        while ( 1 ) {
            if ( stats.nrecords < maxCappedDocs() ) {
                loc = __capAlloc( len );
                if ( !loc.isNull() )
                    break;
            }

            // If on first iteration through extents, don't delete anything.
            if ( !capFirstNewRecord.isValid() ) {
                advanceCapExtent( ns );

                if ( capExtent != firstExtent )
                    capFirstNewRecord.writing().setInvalid();
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
                log() << "passes max:" << maxCappedDocs() << " nrecords:" << stats.nrecords << " datasize: " << stats.datasize << endl;
                massert( 10345 ,  "passes >= maxPasses in capped collection alloc", false );
            }
        }

        // Remember first record allocated on this iteration through capExtent.
        if ( capFirstNewRecord.isValid() && capFirstNewRecord.isNull() )
            getDur().writingDiskLoc(capFirstNewRecord) = loc;

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
        for( DiskLoc z = deletedList[0]; !z.isNull(); z = z.drec()->nextDeleted() ) {
            cout << "  drec:" << z.toString() << " dreclen:" << hex << z.drec()->lengthWithHeaders() <<
                 " ext:" << z.drec()->myExtent(z)->myLoc.toString() << endl;
        }
        cout << "dl[1]: " << deletedList[1].toString() << endl;
    }

    void NamespaceDetails::cappedTruncateLastDelUpdate() {
        if ( capExtent == firstExtent ) {
            // Only one extent of the collection is in use, so there
            // is no deleted record in a previous extent, so nullify
            // cappedLastDelRecLastExtent().
            cappedLastDelRecLastExtent().writing() = DiskLoc();
        }
        else {
            // Scan through all deleted records in the collection
            // until the last deleted record for the extent prior
            // to the new capExtent is found.  Then set
            // cappedLastDelRecLastExtent() to that deleted record.
            DiskLoc i = cappedListOfAllDeletedRecords();
            for( ;
                 !i.drec()->nextDeleted().isNull() &&
                     !inCapExtent( i.drec()->nextDeleted() );
                 i = i.drec()->nextDeleted() );
            // In our capped storage model, every extent must have at least one
            // deleted record.  Here we check that 'i' is not the last deleted
            // record.  (We expect that there will be deleted records in the new
            // capExtent as well.)
            verify( !i.drec()->nextDeleted().isNull() );
            cappedLastDelRecLastExtent().writing() = i;
        }
    }

    void NamespaceDetails::cappedTruncateAfter(const char *ns, DiskLoc end, bool inclusive) {
        DEV verify( this == nsdetails(ns) );
        verify( cappedLastDelRecLastExtent().isValid() );

        // We iteratively remove the newest document until the newest document
        // is 'end', then we remove 'end' if requested.
        bool foundLast = false;
        while( 1 ) {
            if ( foundLast ) {
                // 'end' has been found and removed, so break.
                break;
            }
            getDur().commitIfNeeded();
            // 'curr' will point to the newest document in the collection.
            DiskLoc curr = theCapExtent()->lastRecord;
            verify( !curr.isNull() );
            if ( curr == end ) {
                if ( inclusive ) {
                    // 'end' has been found, so break next iteration.
                    foundLast = true;
                }
                else {
                    // 'end' has been found, so break.
                    break;
                }
            }

            // TODO The algorithm used in this function cannot generate an
            // empty collection, but we could call emptyCappedCollection() in
            // this case instead of asserting.
            uassert( 13415, "emptying the collection is not allowed", stats.nrecords > 1 );

            // Delete the newest record, and coalesce the new deleted
            // record with existing deleted records.
            theDataFileMgr.deleteRecord(ns, curr.rec(), curr, true);
            compact();

            // This is the case where we have not yet had to remove any
            // documents to make room for other documents, and we are allocating
            // documents from free space in fresh extents instead of reusing
            // space from familiar extents.
            if ( !capLooped() ) {

                // We just removed the last record from the 'capExtent', and
                // the 'capExtent' can't be empty, so we set 'capExtent' to
                // capExtent's prev extent.
                if ( theCapExtent()->lastRecord.isNull() ) {
                    verify( !theCapExtent()->xprev.isNull() );
                    // NOTE Because we didn't delete the last document, and
                    // capLooped() is false, capExtent is not the first extent
                    // so xprev will be nonnull.
                    capExtent.writing() = theCapExtent()->xprev;
                    theCapExtent()->assertOk();

                    // update cappedLastDelRecLastExtent()
                    cappedTruncateLastDelUpdate();
                }
                continue;
            }

            // This is the case where capLooped() is true, and we just deleted
            // from capExtent, and we just deleted capFirstNewRecord, which was
            // the last record on the fresh side of capExtent.
            // NOTE In this comparison, curr and potentially capFirstNewRecord
            // may point to invalid data, but we can still compare the
            // references themselves.
            if ( curr == capFirstNewRecord ) {

                // Set 'capExtent' to the first nonempty extent prior to the
                // initial capExtent.  There must be such an extent because we
                // have not deleted the last document in the collection.  It is
                // possible that all extents other than the capExtent are empty.
                // In this case we will keep the initial capExtent and specify
                // that all records contained within are on the fresh rather than
                // stale side of the extent.
                DiskLoc newCapExtent = capExtent;
                do {
                    // Find the previous extent, looping if necessary.
                    newCapExtent = ( newCapExtent == firstExtent ) ? lastExtent : newCapExtent.ext()->xprev;
                    newCapExtent.ext()->assertOk();
                }
                while ( newCapExtent.ext()->firstRecord.isNull() );
                capExtent.writing() = newCapExtent;

                // Place all documents in the new capExtent on the fresh side
                // of the capExtent by setting capFirstNewRecord to the first
                // document in the new capExtent.
                capFirstNewRecord.writing() = theCapExtent()->firstRecord;

                // update cappedLastDelRecLastExtent()
                cappedTruncateLastDelUpdate();
            }
        }
    }

    void NamespaceDetails::emptyCappedCollection( const char *ns ) {
        DEV verify( this == nsdetails(ns) );
        massert( 13424, "collection must be capped", isCapped() );
        massert( 13425, "background index build in progress", !indexBuildInProgress );
        massert( 13426, "indexes present", nIndexes == 0 );

        // Clear all references to this namespace.
        ClientCursor::invalidate( ns );
        NamespaceDetailsTransient::clearForPrefix( ns );

        // Get a writeable reference to 'this' and reset all pertinent
        // attributes.
        NamespaceDetails *t = writingWithoutExtra();

        t->cappedLastDelRecLastExtent() = DiskLoc();
        t->cappedListOfAllDeletedRecords() = DiskLoc();

        // preserve firstExtent/lastExtent
        t->capExtent = firstExtent;
        t->stats.datasize = stats.nrecords = 0;
        // lastExtentSize preserve
        // nIndexes preserve 0
        // capped preserve true
        // max preserve
        t->_paddingFactor = 1.0;
        t->_systemFlags = 0;
        t->capFirstNewRecord = DiskLoc();
        t->capFirstNewRecord.setInvalid();
        t->cappedLastDelRecLastExtent().setInvalid();
        // dataFileVersion preserve
        // indexFileVersion preserve
        t->multiKeyIndexBits = 0;
        t->reservedA = 0;
        t->extraOffset = 0;
        // indexBuildInProgress preserve 0
        memset(t->reserved, 0, sizeof(t->reserved));

        // Reset all existing extents and recreate the deleted list.
        for( DiskLoc ext = firstExtent; !ext.isNull(); ext = ext.ext()->xnext ) {
            DiskLoc prev = ext.ext()->xprev;
            DiskLoc next = ext.ext()->xnext;
            DiskLoc empty = ext.ext()->reuse( ns, true );
            ext.ext()->xprev.writing() = prev;
            ext.ext()->xnext.writing() = next;
            addDeletedRec( empty.drec(), empty );
        }
    }

}
