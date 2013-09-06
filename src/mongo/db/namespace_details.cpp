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

#include "mongo/pch.h"

#include "mongo/db/namespace_details.h"

#include <algorithm>
#include <list>

#include "mongo/db/clientcursor.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/json.h"
#include "mongo/db/storage/durable_mapped_file.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/pdfile.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/hashtab.h"
#include "mongo/util/startup_test.h"


namespace mongo {

    BSONObj idKeyPattern = fromjson("{\"_id\":1}");

    /* Deleted list buckets are used to quickly locate free space based on size.  Each bucket
       contains records up to that size.  All records >= 4mb are placed into the 16mb bucket.
    */
    int bucketSizes[] = {
        0x20,     0x40,     0x80,     0x100,
        0x200,    0x400,    0x800,    0x1000,
        0x2000,   0x4000,   0x8000,   0x10000,
        0x20000,  0x40000,  0x80000,  0x100000,
        0x200000, 0x400000, 0x1000000,
     };

    NamespaceDetails::NamespaceDetails( const DiskLoc &loc, bool capped ) {
        /* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
        _firstExtent = _lastExtent = _capExtent = loc;
        _stats.datasize = _stats.nrecords = 0;
        _lastExtentSize = 0;
        _nIndexes = 0;
        _isCapped = capped;
        _maxDocsInCapped = 0x7fffffff; // no limit (value is for pre-v2.3.2 compatability)
        _paddingFactor = 1.0;
        _systemFlags = 0;
        _userFlags = 0;
        _capFirstNewRecord = DiskLoc();
        // Signal that we are on first allocation iteration through extents.
        _capFirstNewRecord.setInvalid();
        // For capped case, signal that we are doing initial extent allocation.
        if ( capped )
            cappedLastDelRecLastExtent().setInvalid();
        verify( sizeof(_dataFileVersion) == 2 );
        _dataFileVersion = 0;
        _indexFileVersion = 0;
        _multiKeyIndexBits = 0;
        _reservedA = 0;
        _extraOffset = 0;
        _indexBuildsInProgress = 0;
        memset(_reserved, 0, sizeof(_reserved));
    }

#if defined(_DEBUG)
    void NamespaceDetails::dump(const Namespace& k) {
        if( !cmdLine.dur )
            cout << "ns offsets which follow will not display correctly with --journal disabled" << endl;

        size_t ofs = 1; // 1 is sentinel that the find call below failed
        privateViews.find(this, /*out*/ofs);

        cout << "ns" << hex << setw(8) << ofs << ' ';
        cout << k.toString() << '\n';

        if( k.isExtra() ) {
            cout << "ns\t extra" << endl;
            return;
        }

        cout << "ns         " << _firstExtent.toString() << ' ' << _lastExtent.toString() << " nidx:" << _nIndexes << '\n';
        cout << "ns         " << _stats.datasize << ' ' << _stats.nrecords << ' ' << _nIndexes << '\n';
        cout << "ns         " << isCapped() << ' ' << _paddingFactor << ' ' << _systemFlags << ' ' << _userFlags << ' ' << _dataFileVersion << '\n';
        cout << "ns         " << _multiKeyIndexBits << ' ' << _indexBuildsInProgress << '\n';
        cout << "ns         " << (int)_reserved[0] << ' ' << (int)_reserved[59];
        cout << endl;
    }
#endif


    void NamespaceDetails::addDeletedRec(DeletedRecord *d, DiskLoc dloc) {
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails) );

        {
            Record *r = (Record *) getDur().writingPtr(d, sizeof(Record));
            d = &r->asDeleted();
            // defensive code: try to make us notice if we reference a deleted record
            reinterpret_cast<unsigned*>( r->data() )[0] = 0xeeeeeeee;
        }
        DEBUGGING log() << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs() << endl;
        if ( isCapped() ) {
            if ( !cappedLastDelRecLastExtent().isValid() ) {
                // Initial extent allocation.  Insert at end.
                d->nextDeleted() = DiskLoc();
                if ( cappedListOfAllDeletedRecords().isNull() )
                    getDur().writingDiskLoc( cappedListOfAllDeletedRecords() ) = dloc;
                else {
                    DiskLoc i = cappedListOfAllDeletedRecords();
                    for (; !i.drec()->nextDeleted().isNull(); i = i.drec()->nextDeleted() )
                        ;
                    i.drec()->nextDeleted().writing() = dloc;
                }
            }
            else {
                d->nextDeleted() = cappedFirstDeletedInCurExtent();
                getDur().writingDiskLoc( cappedFirstDeletedInCurExtent() ) = dloc;
                // always compact() after this so order doesn't matter
            }
        }
        else {
            int b = bucket(d->lengthWithHeaders());
            DiskLoc& list = _deletedList[b];
            DiskLoc oldHead = list;
            getDur().writingDiskLoc(list) = dloc;
            d->nextDeleted() = oldHead;
        }
    }

    /* @return the size for an allocated record quantized to 1/16th of the BucketSize
       @param allocSize    requested size to allocate
    */
    int NamespaceDetails::quantizeAllocationSpace(int allocSize) {
        const int bucketIdx = bucket(allocSize);
        int bucketSize = bucketSizes[bucketIdx];
        int quantizeUnit = bucketSize / 16;
        if (allocSize >= (1 << 22)) // 4mb
            // all allocatons >= 4mb result in 4mb/16 quantization units, even if >= 8mb.  idea is
            // to reduce quantization overhead of large records at the cost of increasing the
            // DeletedRecord size distribution in the largest bucket by factor of 4.
            quantizeUnit = (1 << 18); // 256k
        if (allocSize % quantizeUnit == 0)
            // size is already quantized
            return allocSize;
        const int quantizedSpace = (allocSize | (quantizeUnit - 1)) + 1;
        fassert(16484, quantizedSpace >= allocSize);
        return quantizedSpace;
    }

    int NamespaceDetails::quantizePowerOf2AllocationSpace(int allocSize) {
        int allocationSize = bucketSizes[ bucket( allocSize ) ];
        if ( allocationSize == bucketSizes[MaxBucket] ) {
            // if we get here, it means we're allocating more than 4mb, so round
            // to the nearest megabyte
            allocationSize = 1 + ( allocSize | ( ( 1 << 20 ) - 1 ) );
        }
        return allocationSize;
    }

    /* predetermine location of the next alloc without actually doing it.
        if cannot predetermine returns null (so still call alloc() then)
    */
    DiskLoc NamespaceDetails::allocWillBeAt(const char *ns, int lenToAlloc) {
        if ( ! isCapped() ) {
            lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
            return __stdAlloc(lenToAlloc, true);
        }
        return DiskLoc();
    }

    /** allocate space for a new record from deleted lists.
        @param lenToAlloc is WITH header
        @return null diskloc if no room - allocate a new extent then
    */
    DiskLoc NamespaceDetails::alloc(const char* ns, int lenToAlloc) {
        {
            // align very slightly.
            lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
        }

        DiskLoc loc = _alloc(ns, lenToAlloc);
        if ( loc.isNull() )
            return loc;

        DeletedRecord *r = loc.drec();
        //r = getDur().writing(r);

        /* note we want to grab from the front so our next pointers on disk tend
        to go in a forward direction which is important for performance. */
        int regionlen = r->lengthWithHeaders();
        verify( r->extentOfs() < loc.getOfs() );

        DEBUGGING out() << "TEMP: alloc() returns " << loc.toString() << ' ' << ns << " lentoalloc:" << lenToAlloc << endl;

        int left = regionlen - lenToAlloc;
        if ( ! isCapped() ) {
            if ( left < 24 || left < (lenToAlloc >> 3) ) {
                // you get the whole thing.
                return loc;
            }
        }

        // don't quantize:
        //   - capped collections: just wastes space
        //   - $ collections (indexes) as we already have those aligned the way we want SERVER-8425
        if ( !isCapped() && NamespaceString::normal( ns ) ) {
            // we quantize here so that it only impacts newly sized records
            // this prevents oddities with older records and space re-use SERVER-8435
            lenToAlloc = std::min( r->lengthWithHeaders(),
                                   NamespaceDetails::quantizeAllocationSpace( lenToAlloc ) );
            left = regionlen - lenToAlloc;

            if ( left < 24 ) {
                // you get the whole thing.
                return loc;
            }
        }

        /* split off some for further use. */
        getDur().writingInt(r->lengthWithHeaders()) = lenToAlloc;
        DiskLoc newDelLoc = loc;
        newDelLoc.inc(lenToAlloc);
        DeletedRecord* newDel = DataFileMgr::getDeletedRecord(newDelLoc);
        DeletedRecord* newDelW = getDur().writing(newDel);
        newDelW->extentOfs() = r->extentOfs();
        newDelW->lengthWithHeaders() = left;
        newDelW->nextDeleted().Null();

        addDeletedRec(newDel, newDelLoc);

        return loc;
    }

    /* for non-capped collections.
       @param peekOnly just look up where and don't reserve
       returned item is out of the deleted list upon return
    */
    DiskLoc NamespaceDetails::__stdAlloc(int len, bool peekOnly) {
        DiskLoc *prev;
        DiskLoc *bestprev = 0;
        DiskLoc bestmatch;
        int bestmatchlen = 0x7fffffff;
        int b = bucket(len);
        DiskLoc cur = _deletedList[b];
        prev = &_deletedList[b];
        int extra = 5; // look for a better fit, a little.
        int chain = 0;
        while ( 1 ) {
            { // defensive check
                int fileNumber = cur.a();
                int fileOffset = cur.getOfs();
                if (fileNumber < -1 || fileNumber >= 100000 || fileOffset < 0) {
                    StringBuilder sb;
                    sb << "Deleted record list corrupted in bucket " << b
                       << ", link number " << chain
                       << ", invalid link is " << cur.toString()
                       << ", throwing Fatal Assertion";
                    problem() << sb.str() << endl;
                    fassertFailed(16469);
                }
            }
            if ( cur.isNull() ) {
                // move to next bucket.  if we were doing "extra", just break
                if ( bestmatchlen < 0x7fffffff )
                    break;
                b++;
                if ( b > MaxBucket ) {
                    // out of space. alloc a new extent.
                    return DiskLoc();
                }
                cur = _deletedList[b];
                prev = &_deletedList[b];
                continue;
            }
            DeletedRecord *r = cur.drec();
            if ( r->lengthWithHeaders() >= len &&
                 r->lengthWithHeaders() < bestmatchlen ) {
                bestmatchlen = r->lengthWithHeaders();
                bestmatch = cur;
                bestprev = prev;
                if (r->lengthWithHeaders() == len)
                    // exact match, stop searching
                    break;
            }
            if ( bestmatchlen < 0x7fffffff && --extra <= 0 )
                break;
            if ( ++chain > 30 && b < MaxBucket ) {
                // too slow, force move to next bucket to grab a big chunk
                //b++;
                chain = 0;
                cur.Null();
            }
            else {
                /*this defensive check only made sense for the mmap storage engine:
                  if ( r->nextDeleted.getOfs() == 0 ) {
                    problem() << "~~ Assertion - bad nextDeleted " << r->nextDeleted.toString() <<
                    " b:" << b << " chain:" << chain << ", fixing.\n";
                    r->nextDeleted.Null();
                }*/
                cur = r->nextDeleted();
                prev = &r->nextDeleted();
            }
        }

        /* unlink ourself from the deleted list */
        if( !peekOnly ) {
            DeletedRecord *bmr = bestmatch.drec();
            *getDur().writing(bestprev) = bmr->nextDeleted();
            bmr->nextDeleted().writing().setInvalid(); // defensive.
            verify(bmr->extentOfs() < bestmatch.getOfs());
        }

        return bestmatch;
    }

    void NamespaceDetails::dumpDeleted(set<DiskLoc> *extents) {
        for ( int i = 0; i < Buckets; i++ ) {
            DiskLoc dl = _deletedList[i];
            while ( !dl.isNull() ) {
                DeletedRecord *r = dl.drec();
                DiskLoc extLoc(dl.a(), r->extentOfs());
                if ( extents == 0 || extents->count(extLoc) <= 0 ) {
                    out() << "  bucket " << i << endl;
                    out() << "   " << dl.toString() << " ext:" << extLoc.toString();
                    if ( extents && extents->count(extLoc) <= 0 )
                        out() << '?';
                    out() << " len:" << r->lengthWithHeaders() << endl;
                }
                dl = r->nextDeleted();
            }
        }
    }

    DiskLoc NamespaceDetails::firstRecord( const DiskLoc &startExtent ) const {
        for (DiskLoc i = startExtent.isNull() ? _firstExtent : startExtent;
                !i.isNull(); i = i.ext()->xnext ) {
            if ( !i.ext()->firstRecord.isNull() )
                return i.ext()->firstRecord;
        }
        return DiskLoc();
    }

    DiskLoc NamespaceDetails::lastRecord( const DiskLoc &startExtent ) const {
        for (DiskLoc i = startExtent.isNull() ? _lastExtent : startExtent;
                !i.isNull(); i = i.ext()->xprev ) {
            if ( !i.ext()->lastRecord.isNull() )
                return i.ext()->lastRecord;
        }
        return DiskLoc();
    }

    int n_complaints_cap = 0;
    void NamespaceDetails::maybeComplain( const char *ns, int len ) const {
        if ( ++n_complaints_cap < 8 ) {
            out() << "couldn't make room for new record (len: " << len << ") in capped ns " << ns << '\n';
            int i = 0;
            for ( DiskLoc e = _firstExtent; !e.isNull(); e = e.ext()->xnext, ++i ) {
                out() << "  Extent " << i;
                if ( e == _capExtent )
                    out() << " (capExtent)";
                out() << '\n';
                out() << "    magic: " << hex << e.ext()->magic << dec << " extent->ns: " << e.ext()->nsDiagnostic.toString() << '\n';
                out() << "    fr: " << e.ext()->firstRecord.toString() <<
                      " lr: " << e.ext()->lastRecord.toString() << " extent->len: " << e.ext()->length << '\n';
            }
            verify( len * 5 > _lastExtentSize ); // assume it is unusually large record; if not, something is broken
        }
    }

    /* alloc with capped table handling. */
    DiskLoc NamespaceDetails::_alloc(const char *ns, int len) {
        if ( ! isCapped() )
            return __stdAlloc(len, false);

        return cappedAlloc(ns,len);
    }

    NamespaceDetails::Extra* NamespaceDetails::allocExtra(const char *ns, int nindexessofar) {
        Lock::assertWriteLocked(ns);

        NamespaceIndex *ni = nsindex(ns);

        int i = (nindexessofar - NIndexesBase) / NIndexesExtra;
        verify( i >= 0 && i <= 1 );

        Namespace fullns(ns);
        Namespace extrans(fullns.extraName(i)); // throws userexception if ns name too long

        massert( 10350 ,  "allocExtra: base ns missing?", this );
        massert( 10351 ,  "allocExtra: extra already exists", ni->details(extrans) == 0 );

        Extra temp;
        temp.init();

        ni->add_ns( extrans, reinterpret_cast<NamespaceDetails*>( &temp ) );
        Extra* e = reinterpret_cast<NamespaceDetails::Extra*>( ni->details( extrans ) );

        long ofs = e->ofsFrom(this);
        if( i == 0 ) {
            verify( _extraOffset == 0 );
            *getDur().writing(&_extraOffset) = ofs;
            verify( extra() == e );
        }
        else {
            Extra *hd = extra();
            verify( hd->next(this) == 0 );
            hd->setNext(ofs);
        }
        return e;
    }

    void NamespaceDetails::setIndexIsMultikey(const char *thisns, int i, bool multikey) {
        massert(16577, "index number greater than NIndexesMax", i < NIndexesMax );

        unsigned long long mask = 1ULL << i;

        if (multikey) {
            // Shortcut if the bit is already set correctly
            if (_multiKeyIndexBits & mask) {
                return;
            }

            *getDur().writing(&_multiKeyIndexBits) |= mask;
        }
        else {
            // Shortcut if the bit is already set correctly
            if (!(_multiKeyIndexBits & mask)) {
                return;
            }

            // Invert mask: all 1's except a 0 at the ith bit
            mask = ~mask;
            *getDur().writing(&_multiKeyIndexBits) &= mask;
        }

        NamespaceDetailsTransient::get(thisns).clearQueryCache();
    }

    IndexDetails& NamespaceDetails::getNextIndexDetails(const char* thisns) {
        IndexDetails *id;
        try {
            id = &idx(getTotalIndexCount(), true);
        }
        catch(DBException&) {
            allocExtra(thisns, getTotalIndexCount());
            id = &idx(getTotalIndexCount(), false);
        }
        return *id;
    }

    /* you MUST call when adding an index.  see pdfile.cpp */
    void NamespaceDetails::addIndex(const char* thisns) {
        NamespaceDetailsTransient::get(thisns).addedIndex();
        (*getDur().writing(&_nIndexes))++;
    }

    // must be called when renaming a NS to fix up extra
    void NamespaceDetails::copyingFrom(const char *thisns, NamespaceDetails *src) {
        _extraOffset = 0; // we are a copy -- the old value is wrong.  fixing it up below.
        Extra *se = src->extra();
        int n = NIndexesBase;
        if( se ) {
            Extra *e = allocExtra(thisns, n);
            while( 1 ) {
                n += NIndexesExtra;
                e->copy(this, *se);
                se = se->next(src);
                if( se == 0 ) break;
                Extra *nxt = allocExtra(thisns, n);
                e->setNext( nxt->ofsFrom(this) );
                e = nxt;
            }
            verify( _extraOffset );
        }
    }

    /* returns index of the first index in which the field is present. -1 if not present.
       (aug08 - this method not currently used)
    */
    int NamespaceDetails::fieldIsIndexed(const char *fieldName) {
        massert( 10346 , "not implemented", false);
        /*
        for ( int i = 0; i < nIndexes; i++ ) {
            IndexDetails& idx = indexes[i];
            BSONObj idxKey = idx.info.obj().getObjectField("key"); // e.g., { ts : -1 }
            if ( !idxKey.getField(fieldName).eoo() )
                return i;
        }*/
        return -1;
    }

    long long NamespaceDetails::storageSize( int * numExtents , BSONArrayBuilder * extentInfo ) const {
        Extent * e = _firstExtent.ext();
        verify( e );

        long long total = 0;
        int n = 0;
        while ( e ) {
            total += e->length;
            n++;

            if ( extentInfo ) {
                extentInfo->append( BSON( "len" << e->length << "loc: " << e->myLoc.toBSONObj() ) );
            }

            e = e->getNextExtent();
        }

        if ( numExtents )
            *numExtents = n;

        return total;
    }

    NamespaceDetails *NamespaceDetails::writingWithExtra() {
        vector< pair< long long, unsigned > > writeRanges;
        writeRanges.push_back( make_pair( 0, sizeof( NamespaceDetails ) ) );
        for( Extra *e = extra(); e; e = e->next( this ) ) {
            writeRanges.push_back( make_pair( (char*)e - (char*)this, sizeof( Extra ) ) );
        }
        return reinterpret_cast< NamespaceDetails* >( getDur().writingRangesAtOffsets( this, writeRanges ) );
    }

    void NamespaceDetails::setMaxCappedDocs( long long max ) {
        massert( 16499,
                 "max in a capped collection has to be < 2^31 or -1",
                 validMaxCappedDocs( &max ) );
        _maxDocsInCapped = max;
    }

    bool NamespaceDetails::validMaxCappedDocs( long long* max ) {
        if ( *max <= 0 ||
             *max == numeric_limits<long long>::max() ) {
            *max = 0x7fffffff;
            return true;
        }

        if ( *max < ( 0x1LL << 31 ) ) {
            return true;
        }

        return false;
    }

    long long NamespaceDetails::maxCappedDocs() const {
        verify( isCapped() );
        if ( _maxDocsInCapped == 0x7fffffff )
            return numeric_limits<long long>::max();
        return _maxDocsInCapped;
    }

    /* ------------------------------------------------------------------------- */

    SimpleMutex NamespaceDetailsTransient::_qcMutex("qc");
    NamespaceDetailsTransient::DMap NamespaceDetailsTransient::_nsdMap;

    void NamespaceDetailsTransient::reset() {
        Lock::assertWriteLocked(_ns); 
        clearQueryCache();
        _keysComputed = false;
    }

    NamespaceDetailsTransient::CMap& NamespaceDetailsTransient::get_cmap_inlock(const string& ns) {
        CMap*& m = _nsdMap[ns];
        if ( ! m )
            m = new CMap();
        return *m;
    }

    /*static*/ NOINLINE_DECL NamespaceDetailsTransient& NamespaceDetailsTransient::make_inlock(const string& ns) {
        shared_ptr< NamespaceDetailsTransient > &t = get_cmap_inlock(ns)[ ns ];
        verify( t.get() == 0 );
        Database *database = cc().database();
        verify( database );
        if( _nsdMap.size() % 20000 == 10000 ) { 
            // so we notice if insanely large #s
            log() << "opening namespace " << ns << endl;
            log() << _nsdMap.size() << " namespaces in nsdMap" << endl;
        }
        t.reset( new NamespaceDetailsTransient(database, ns) );
        return *t;
    }

    // note with repair there could be two databases with the same ns name.
    // that is NOT handled here yet!  TODO
    // repair may not use nsdt though not sure.  anyway, requires work.
    NamespaceDetailsTransient::NamespaceDetailsTransient(Database *db, const string& ns) : 
        _ns(ns), _keysComputed(false), _qcWriteCount() 
    {
        dassert(db);
    }

    NamespaceDetailsTransient::~NamespaceDetailsTransient() { 
    }
    
    void NamespaceDetailsTransient::resetCollection(const string& ns ) {
        SimpleMutex::scoped_lock lk(_qcMutex);
        Lock::assertWriteLocked(ns);
        get_cmap_inlock(ns)[ns].reset();
    }
        
    void NamespaceDetailsTransient::eraseDB(const string& db) {
        SimpleMutex::scoped_lock lk(_qcMutex);
        Lock::assertWriteLocked(db);
        
        DMap::iterator i = _nsdMap.find( db );
        if ( i != _nsdMap.end() ) {
            delete i->second;
            _nsdMap.erase( i );
        }
    }
    
    void NamespaceDetailsTransient::eraseCollection(const string& ns) {
        SimpleMutex::scoped_lock lk(_qcMutex);
        Lock::assertWriteLocked(ns);
        get_cmap_inlock(ns).erase(ns);
    }


    void NamespaceDetailsTransient::computeIndexKeys() {
        _indexedPaths.clear();

        NamespaceDetails *d = nsdetails(_ns);
        if ( ! d )
            return;

        NamespaceDetails::IndexIterator i = d->ii( true );
        while( i.more() ) {
            BSONObj key = i.next().keyPattern();
            BSONObjIterator j( key );
            while ( j.more() ) {
                BSONElement e = j.next();
                _indexedPaths.addPath( e.fieldName() );
            }
        }

        _keysComputed = true;
    }

    void NamespaceDetails::updateTTLIndex( int idxNo , const BSONElement& newExpireSecs ) {
        // Need to get the actual DiskLoc of the index to update. This is embedded in the 'info'
        // object inside the IndexDetails.
        IndexDetails idetails = idx( idxNo );
        BSONElement oldExpireSecs = idetails.info.obj().getField("expireAfterSeconds");

        // Important that we set the new value in-place.  We are writing directly to the
        // object here so must be careful not to overwrite with a longer numeric type.
        massert( 16630, "new 'expireAfterSeconds' must be a number", newExpireSecs.isNumber() );
        BSONElementManipulator manip( oldExpireSecs );
        switch( oldExpireSecs.type() ) {
        case EOO:
            massert( 16631, "index does not have an 'expireAfterSeconds' field", false );
            break;
        case NumberInt:
        case NumberDouble:
            manip.SetNumber( newExpireSecs.numberDouble() );
            break;
        case NumberLong:
            manip.SetLong( newExpireSecs.numberLong() );
            break;
        default:
            massert( 16632, "current 'expireAfterSeconds' is not a number", false );
        }
    }

    void NamespaceDetails::setSystemFlag( int flag ) {
        getDur().writingInt(_systemFlags) |= flag;
    }

    void NamespaceDetails::clearSystemFlag( int flag ) {
        getDur().writingInt(_systemFlags) &= ~flag;
    }

    void NamespaceDetails::setLastExtentSize( int newMax ) {
        if ( _lastExtentSize == newMax )
            return;
        getDur().writingInt(_lastExtentSize) = newMax;
    }

    void NamespaceDetails::incrementStats( long long dataSizeIncrement,
                                           long long numRecordsIncrement ) {

        // durability todo : this could be a bit annoying / slow to record constantly
        Stats* s = getDur().writing( &_stats );
        s->datasize += dataSizeIncrement;
        s->nrecords += numRecordsIncrement;
    }

    void NamespaceDetails::setStats( long long dataSize,
                                     long long numRecords ) {
        Stats* s = getDur().writing( &_stats );
        s->datasize = dataSize;
        s->nrecords = numRecords;
    }


    void NamespaceDetails::setFirstExtent( DiskLoc newFirstExtent ) {
        getDur().writingDiskLoc( _firstExtent ) = newFirstExtent;
    }

    void NamespaceDetails::setLastExtent( DiskLoc newLastExtent ) {
        getDur().writingDiskLoc( _lastExtent ) = newLastExtent;
    }

    void NamespaceDetails::setFirstExtentInvalid() {
        getDur().writingDiskLoc( _firstExtent ).setInvalid();
    }

    void NamespaceDetails::setLastExtentInvalid() {
        getDur().writingDiskLoc( _lastExtent ).setInvalid();
    }


    /**
     * keeping things in sync this way is a bit of a hack
     * and the fact that we have to pass in ns again
     * should be changed, just not sure to what
     */
    void NamespaceDetails::syncUserFlags( const string& ns ) {
        Lock::assertWriteLocked( ns );
        
        string system_namespaces = nsToDatabaseSubstring(ns).toString() + ".system.namespaces";

        BSONObj oldEntry;
        verify( Helpers::findOne( system_namespaces , BSON( "name" << ns ) , oldEntry ) );
        BSONObj newEntry = applyUpdateOperators( oldEntry , BSON( "$set" << BSON( "options.flags" << userFlags() ) ) );
        
        verify( 1 == deleteObjects( system_namespaces.c_str() , oldEntry , true , false , true ) );
        theDataFileMgr.insert( system_namespaces.c_str(),
                               newEntry.objdata(),
                               newEntry.objsize(),
                               false,
                               true );
    }

    bool NamespaceDetails::setUserFlag( int flags ) {
        if ( ( _userFlags & flags ) == flags )
            return false;
        
        getDur().writingInt(_userFlags) |= flags;
        return true;
    }

    bool NamespaceDetails::clearUserFlag( int flags ) {
        if ( ( _userFlags & flags ) == 0 )
            return false;

        getDur().writingInt(_userFlags) &= ~flags;
        return true;
    }

    bool NamespaceDetails::replaceUserFlags( int flags ) {
        if ( flags == _userFlags )
            return false;

        getDur().writingInt(_userFlags) = flags;
        return true;
    }



    int NamespaceDetails::getRecordAllocationSize( int minRecordSize ) {

        if ( isCapped() )
            return minRecordSize;

        if ( _paddingFactor == 0 ) {
            warning() << "implicit updgrade of paddingFactor of very old collection" << endl;
            setPaddingFactor(1.0);
        }
        verify( _paddingFactor >= 1 );

        if ( isUserFlagSet( Flag_UsePowerOf2Sizes ) ) {
            // quantize to the nearest bucketSize (or nearest 1mb boundary for large sizes).
            return quantizePowerOf2AllocationSpace(minRecordSize);
        }

        // adjust for padding factor
        return static_cast<int>(minRecordSize * _paddingFactor);
    }

    NamespaceDetails::IndexBuildBlock::IndexBuildBlock( const string& ns, const string& indexName )
        : _ns( ns ), _indexName( indexName ) {

        NamespaceDetails* nsd = nsdetails( _ns );
        verify( nsd );
        getDur().writingInt( nsd->_indexBuildsInProgress ) += 1;
    }

    NamespaceDetails::IndexBuildBlock::~IndexBuildBlock() {
        NamespaceDetails* nsd = nsdetails( _ns );
        if ( nsd ) {
            getDur().writingInt( nsd->_indexBuildsInProgress ) -= 1;
        }
    }

    /* remove bit from a bit array - actually remove its slot, not a clear
       note: this function does not work with x == 63 -- that is ok
             but keep in mind in the future if max indexes were extended to
             exactly 64 it would be a problem
    */
    unsigned long long removeAndSlideBit(unsigned long long b, int x) {
        unsigned long long tmp = b;
        return
            (tmp & ((((unsigned long long) 1) << x)-1)) |
            ((tmp >> (x+1)) << x);
    }

    void NamespaceDetails::removeIndex( int idxNumber ) {
        verify( idxNumber >= 0 );
        verify( idxNumber < _nIndexes );
        verify( _indexBuildsInProgress == 0 );

        /* note it is  important we remove the IndexDetails with this
           call, otherwise, on recreate, the old one would be reused, and its
           IndexDetails::info ptr would be bad info.
        */

        aboutToDeleteAnIndex();

        _removeIndex( idxNumber );
    }

    void NamespaceDetails::_removeIndex( int idxNumber ) {

        // TODO: don't do this whole thing, do it piece meal for readability
        NamespaceDetails* d = writingWithExtra();

        IndexDetails *id = &d->idx(idxNumber);
        id->kill_idx();

        // fix the _multiKeyIndexBits, by moving all bits above me down one
        d->_multiKeyIndexBits = removeAndSlideBit(d->_multiKeyIndexBits, idxNumber);

        if ( idxNumber >= _nIndexes )
            d->_indexBuildsInProgress--;
        else
            d->_nIndexes--;

        for ( int i = idxNumber; i < getTotalIndexCount(); i++ )
            d->idx(i) = d->idx(i+1);

        d->idx( getTotalIndexCount() ) = IndexDetails();
    }

    BSONObj NamespaceDetails::prepOneUnfinishedIndex() {
        verify( _indexBuildsInProgress > 0 );

        // details.info is always a valid system.indexes entry because DataFileMgr::insert journals
        // creating the index doc and then insert_makeIndex durably assigns its DiskLoc to info.
        // indexBuildsInProgress is set after that, so if it is set, info must be set.
        int offset = getTotalIndexCount() - 1;

        BSONObj info = idx(offset).info.obj().getOwned();

        _removeIndex( offset );

        return info;
    }

    void NamespaceDetails::blowAwayInProgressIndexEntries() {
        int inProg = _indexBuildsInProgress;

        getDur().writingInt(_indexBuildsInProgress) = 0;

        for (int i = 0; i < inProg; i++) {
            idx( _nIndexes + i ).kill_idx();
        }

    }

    void NamespaceDetails::swapIndex( const char* ns, int a, int b ) {

        // flip main meta data
        IndexDetails temp = idx(a);
        *getDur().writing(&idx(a)) = idx(b);
        *getDur().writing(&idx(b)) = temp;

        // flip multi key bits
        bool tempMultikey = isMultikey(a);
        setIndexIsMultikey( ns, a, isMultikey(b) );
        setIndexIsMultikey( ns, b, tempMultikey );
    }

    void NamespaceDetails::orphanDeletedList() {
        for( int i = 0; i < Buckets; i++ ) {
            _deletedList[i].writing().Null();
        }
    }

    /* ------------------------------------------------------------------------- */

    /* add a new namespace to the system catalog (<dbname>.system.namespaces).
       options: { capped : ..., size : ... }
    */
    void addNewNamespaceToCatalog(const char *ns, const BSONObj *options = 0) {
        LOG(1) << "New namespace: " << ns << endl;
        if ( strstr(ns, "system.namespaces") ) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            // TODO: fix above should not be strstr!
            return;
        }
        
        BSONObjBuilder b;
        b.append("name", ns);
        if ( options )
            b.append("options", *options);
        BSONObj j = b.done();
        char database[256];
        nsToDatabase(ns, database);
        string s = string(database) + ".system.namespaces";
        theDataFileMgr.insert(s.c_str(), j.objdata(), j.objsize(), false, true);
    }


    bool legalClientSystemNS( const string& ns , bool write ) {
        if( ns == "local.system.replset" ) return true;

        if ( ns.find( ".system.users" ) != string::npos )
            return true;

        if ( ns.find( ".system.js" ) != string::npos ) {
            if ( write )
                Scope::storedFuncMod();
            return true;
        }

        return false;
    }

    class IndexUpdateTest : public StartupTest {
    public:
        void run() {
            verify( removeAndSlideBit(1, 0) == 0 );
            verify( removeAndSlideBit(2, 0) == 1 );
            verify( removeAndSlideBit(2, 1) == 0 );
            verify( removeAndSlideBit(255, 1) == 127 );
            verify( removeAndSlideBit(21, 2) == 9 );
            verify( removeAndSlideBit(0x4000000000000001ULL, 62) == 1 );
        }
    } iu_unittest;

} // namespace mongo
