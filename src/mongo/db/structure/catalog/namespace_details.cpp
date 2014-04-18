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

#include "mongo/db/structure/catalog/namespace_details.h"

#include <algorithm>
#include <list>

#include "mongo/base/counter.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/storage/durable_mapped_file.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/structure/catalog/hashtab.h"
#include "mongo/scripting/engine.h"
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
        if ( capped ) {
            // WAS: cappedLastDelRecLastExtent().setInvalid();
            _deletedList[1].setInvalid();
        }
        verify( sizeof(_dataFileVersion) == 2 );
        _dataFileVersion = 0;
        _indexFileVersion = 0;
        _multiKeyIndexBits = 0;
        _reservedA = 0;
        _extraOffset = 0;
        _indexBuildsInProgress = 0;
        memset(_reserved, 0, sizeof(_reserved));
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

    NamespaceDetails::Extra* NamespaceDetails::allocExtra( const StringData& ns,
                                                           NamespaceIndex& ni,
                                                           int nindexessofar) {
        Lock::assertWriteLocked(ns);

        int i = (nindexessofar - NIndexesBase) / NIndexesExtra;
        verify( i >= 0 && i <= 1 );

        Namespace fullns( ns );
        Namespace extrans( fullns.extraName(i) ); // throws UserException if ns name too long

        massert( 10350, "allocExtra: base ns missing?", this );
        massert( 10351, "allocExtra: extra already exists", ni.details(extrans) == 0 );

        Extra temp;
        temp.init();

        ni.add_ns( extrans, reinterpret_cast<NamespaceDetails*>( &temp ) );
        Extra* e = reinterpret_cast<NamespaceDetails::Extra*>( ni.details( extrans ) );

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

    bool NamespaceDetails::setIndexIsMultikey(int i, bool multikey) {
        massert(16577, "index number greater than NIndexesMax", i < NIndexesMax );

        unsigned long long mask = 1ULL << i;

        if (multikey) {
            // Shortcut if the bit is already set correctly
            if (_multiKeyIndexBits & mask) {
                return false;
            }

            *getDur().writing(&_multiKeyIndexBits) |= mask;
        }
        else {
            // Shortcut if the bit is already set correctly
            if (!(_multiKeyIndexBits & mask)) {
                return false;
            }

            // Invert mask: all 1's except a 0 at the ith bit
            mask = ~mask;
            *getDur().writing(&_multiKeyIndexBits) &= mask;
        }

        return true;
    }

    IndexDetails& NamespaceDetails::getNextIndexDetails(Collection* collection) {
        IndexDetails *id;
        try {
            id = &idx(getTotalIndexCount(), true);
        }
        catch(DBException&) {
            allocExtra(collection->ns().ns(),
                       collection->_database->namespaceIndex(),
                       getTotalIndexCount());
            id = &idx(getTotalIndexCount(), false);
        }
        return *id;
    }

    IndexDetails& NamespaceDetails::idx(int idxNo, bool missingExpected) {
        if( idxNo < NIndexesBase ) {
            IndexDetails& id = _indexes[idxNo];
            return id;
        }
        Extra *e = extra();
        if ( ! e ) {
            if ( missingExpected )
                throw MsgAssertionException( 13283 , "Missing Extra" );
            massert(14045, "missing Extra", e);
        }
        int i = idxNo - NIndexesBase;
        if( i >= NIndexesExtra ) {
            e = e->next(this);
            if ( ! e ) {
                if ( missingExpected )
                    throw MsgAssertionException( 14823 , "missing extra" );
                massert(14824, "missing Extra", e);
            }
            i -= NIndexesExtra;
        }
        return e->details[i];
    }


    const IndexDetails& NamespaceDetails::idx(int idxNo, bool missingExpected) const {
        if( idxNo < NIndexesBase ) {
            const IndexDetails& id = _indexes[idxNo];
            return id;
        }
        const Extra *e = extra();
        if ( ! e ) {
            if ( missingExpected )
                throw MsgAssertionException( 17421 , "Missing Extra" );
            massert(17422, "missing Extra", e);
        }
        int i = idxNo - NIndexesBase;
        if( i >= NIndexesExtra ) {
            e = e->next(this);
            if ( ! e ) {
                if ( missingExpected )
                    throw MsgAssertionException( 17423 , "missing extra" );
                massert(17424, "missing Extra", e);
            }
            i -= NIndexesExtra;
        }
        return e->details[i];
    }


    NamespaceDetails::IndexIterator::IndexIterator(const NamespaceDetails *_d,
                                                   bool includeBackgroundInProgress) {
        d = _d;
        i = 0;
        n = includeBackgroundInProgress ? d->getTotalIndexCount() : d->_nIndexes;
    }

    // must be called when renaming a NS to fix up extra
    void NamespaceDetails::copyingFrom( const char* thisns,
                                        NamespaceIndex& ni,
                                        NamespaceDetails* src) {
        _extraOffset = 0; // we are a copy -- the old value is wrong.  fixing it up below.
        Extra *se = src->extra();
        int n = NIndexesBase;
        if( se ) {
            Extra *e = allocExtra(thisns, ni, n);
            while( 1 ) {
                n += NIndexesExtra;
                e->copy(this, *se);
                se = se->next(src);
                if( se == 0 ) break;
                Extra *nxt = allocExtra(thisns, ni, n);
                e->setNext( nxt->ofsFrom(this) );
                e = nxt;
            }
            verify( _extraOffset );
        }
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
     * // TODO: this should move to Collection
     * keeping things in sync this way is a bit of a hack
     * and the fact that we have to pass in ns again
     * should be changed, just not sure to what
     */
    void NamespaceDetails::syncUserFlags( const string& ns ) {
        Lock::assertWriteLocked( ns );

        string system_namespaces = nsToDatabaseSubstring(ns).toString() + ".system.namespaces";
        Collection* coll = cc().getContext()->db()->getCollection( system_namespaces );

        DiskLoc oldLocation = Helpers::findOne( coll, BSON( "name" << ns ), false );
        fassert( 17247, !oldLocation.isNull() );

        BSONObj oldEntry = coll->docFor( oldLocation );

        BSONObj newEntry = applyUpdateOperators( oldEntry , BSON( "$set" << BSON( "options.flags" << userFlags() ) ) );

        StatusWith<DiskLoc> loc = coll->updateDocument( oldLocation, newEntry, false, NULL );
        if ( !loc.isOK() ) {
            // TODO: should this be an fassert?
            error() << "syncUserFlags failed! "
                    << " ns: " << ns
                    << " error: " << loc.toString();
        }

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

    void NamespaceDetails::setPaddingFactor( double paddingFactor ) {
        if ( paddingFactor == _paddingFactor )
            return;

        if ( isCapped() )
            return;

        *getDur().writing(&_paddingFactor) = paddingFactor;
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

    void NamespaceDetails::_removeIndexFromMe( int idxNumber ) {

        // TODO: don't do this whole thing, do it piece meal for readability
        NamespaceDetails* d = writingWithExtra();

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

    void NamespaceDetails::swapIndex( int a, int b ) {

        // flip main meta data
        IndexDetails temp = idx(a);
        *getDur().writing(&idx(a)) = idx(b);
        *getDur().writing(&idx(b)) = temp;

        // flip multi key bits
        bool tempMultikey = isMultikey(a);
        setIndexIsMultikey( a, isMultikey(b) );
        setIndexIsMultikey( b, tempMultikey );
    }

    void NamespaceDetails::orphanDeletedList() {
        for( int i = 0; i < Buckets; i++ ) {
            _deletedList[i].writing().Null();
        }
    }

    int NamespaceDetails::_catalogFindIndexByName(const StringData& name,
                                                  bool includeBackgroundInProgress) const {
        IndexIterator i = ii(includeBackgroundInProgress);
        while( i.more() ) {
            if ( name == i.next().info.obj().getStringField("name") )
                return i.pos()-1;
        }
        return -1;
    }

    /* ------------------------------------------------------------------------- */

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
