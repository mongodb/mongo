// mmap_v1_extent_manager.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <boost/filesystem/operations.hpp>

#include "mongo/db/storage/mmap_v1/mmap_v1_extent_manager.h"

#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/data_file.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"

namespace mongo {

    MmapV1ExtentManager::MmapV1ExtentManager( const StringData& dbname,
                                  const StringData& path,
                                  bool directoryPerDB )
        : _dbname( dbname.toString() ),
          _path( path.toString() ),
          _directoryPerDB( directoryPerDB ) {
    }

    MmapV1ExtentManager::~MmapV1ExtentManager() {
        reset();
    }

    void MmapV1ExtentManager::reset() {
        for ( size_t i = 0; i < _files.size(); i++ ) {
            delete _files[i];
        }
        _files.clear();
    }

    boost::filesystem::path MmapV1ExtentManager::fileName( int n ) const {
        stringstream ss;
        ss << _dbname << '.' << n;
        boost::filesystem::path fullName( _path );
        if ( _directoryPerDB )
            fullName /= _dbname;
        fullName /= ss.str();
        return fullName;
    }


    Status MmapV1ExtentManager::init(OperationContext* txn) {
        verify( _files.size() == 0 );

        for ( int n = 0; n < DiskLoc::MaxFiles; n++ ) {
            boost::filesystem::path fullName = fileName( n );
            if ( !boost::filesystem::exists( fullName ) )
                break;

            string fullNameString = fullName.string();

            auto_ptr<DataFile> df( new DataFile(n) );

            Status s = df->openExisting( txn, fullNameString.c_str() );

            // openExisting may upgrade the files, so make sure to commit its changes
            txn->recoveryUnit()->commitIfNeeded(true);

            if ( !s.isOK() ) {
                return s;
            }

            if ( df->getHeader()->uninitialized() ) {
                // pre-alloc only, so we're done
                break;
            }

            _files.push_back( df.release() );
        }

        return Status::OK();
    }

    const DataFile* MmapV1ExtentManager::_getOpenFile( int n ) const {
        if ( n < 0 || n >= static_cast<int>(_files.size()) )
            log() << "uh oh: " << n;
        invariant(n >= 0 && n < static_cast<int>(_files.size()));
        return _files[n];
    }


    // todo: this is called a lot. streamline the common case
    DataFile* MmapV1ExtentManager::getFile( OperationContext* txn,
                                      int n,
                                      int sizeNeeded ,
                                      bool preallocateOnly) {
        verify(this);
        DEV txn->lockState()->assertAtLeastReadLocked( _dbname );

        if ( n < 0 || n >= DiskLoc::MaxFiles ) {
            log() << "getFile(): n=" << n << endl;
            massert( 10295 , "getFile(): bad file number value (corrupt db?)."
                    " See http://dochub.mongodb.org/core/data-recovery", false);
        }
        DEV {
            if ( n > 100 ) {
                log() << "getFile(): n=" << n << endl;
            }
        }
        DataFile* p = 0;
        if ( !preallocateOnly ) {
            while ( n >= (int) _files.size() ) {
                verify(this);
                if (!txn->lockState()->isWriteLocked(_dbname)) {
                    log() << "error: getFile() called in a read lock, yet file to return is not yet open";
                    log() << "       getFile(" << n << ") _files.size:" <<_files.size() << ' ' << fileName(n).string();
                    invariant(false);
                }
                _files.push_back(0);
            }
            p = _files[n];
        }
        if ( p == 0 ) {
            if ( n == 0 ) audit::logCreateDatabase( currentClient.get(), _dbname );
            DEV txn->lockState()->assertWriteLocked( _dbname );
            boost::filesystem::path fullName = fileName( n );
            string fullNameString = fullName.string();
            p = new DataFile(n);
            int minSize = 0;
            if ( n != 0 && _files[ n - 1 ] )
                minSize = _files[ n - 1 ]->getHeader()->fileLength;
            if ( sizeNeeded + DataFileHeader::HeaderSize > minSize )
                minSize = sizeNeeded + DataFileHeader::HeaderSize;
            try {
                Timer t;
                p->open( txn, fullNameString.c_str(), minSize, preallocateOnly );
                if ( t.seconds() > 1 ) {
                    log() << "MmapV1ExtentManager took " << t.seconds()
                          << " seconds to open: " << fullNameString;
                }
            }
            catch ( AssertionException& ) {
                delete p;
                throw;
            }
            if ( preallocateOnly )
                delete p;
            else
                _files[n] = p;
        }
        return preallocateOnly ? 0 : p;
    }

    DataFile* MmapV1ExtentManager::_addAFile( OperationContext* txn,
                                        int sizeNeeded,
                                        bool preallocateNextFile ) {
        DEV txn->lockState()->assertWriteLocked(_dbname);
        int n = (int) _files.size();
        DataFile *ret = getFile( txn, n, sizeNeeded );
        if ( preallocateNextFile )
            getFile( txn, numFiles() , 0, true ); // preallocate a file
        return ret;
    }

    int MmapV1ExtentManager::numFiles() const {
        return static_cast<int>( _files.size() );
    }

    long long MmapV1ExtentManager::fileSize() const {
        long long size=0;
        for ( int n = 0; boost::filesystem::exists( fileName(n) ); n++)
            size += boost::filesystem::file_size( fileName(n) );
        return size;
    }

    Record* MmapV1ExtentManager::recordForV1( const DiskLoc& loc ) const {
        loc.assertOk();
        const DataFile* df = _getOpenFile( loc.a() );

        int ofs = loc.getOfs();
        if ( ofs < DataFileHeader::HeaderSize ) {
            df->badOfs(ofs); // will uassert - external call to keep out of the normal code path
        }

        return reinterpret_cast<Record*>( df->p() + ofs );
    }

    DiskLoc MmapV1ExtentManager::extentLocForV1( const DiskLoc& loc ) const {
        Record* record = recordForV1( loc );
        return DiskLoc( loc.a(), record->extentOfs() );
    }

    Extent* MmapV1ExtentManager::extentForV1( const DiskLoc& loc ) const {
        DiskLoc extentLoc = extentLocForV1( loc );
        return getExtent( extentLoc );
    }

    Extent* MmapV1ExtentManager::getExtent( const DiskLoc& loc, bool doSanityCheck ) const {
        loc.assertOk();
        Extent* e = reinterpret_cast<Extent*>( _getOpenFile( loc.a() )->p() + loc.getOfs() );
        if ( doSanityCheck )
            e->assertOk();
        return e;
    }

    void _checkQuota( bool enforceQuota, int fileNo ) {
        if ( !enforceQuota )
            return;

        if ( fileNo < storageGlobalParams.quotaFiles )
            return;

        // exceeded!
        if ( cc().hasWrittenSinceCheckpoint() ) {
            warning() << "quota exceeded, but can't assert" << endl;
            return;
        }

        uasserted(12501, "quota exceeded");
    }

    int MmapV1ExtentManager::maxSize() const {
        return DataFile::maxSize() - DataFileHeader::HeaderSize - 16;
    }

    DiskLoc MmapV1ExtentManager::_createExtentInFile( OperationContext* txn,
                                                int fileNo,
                                                DataFile* f,
                                                int size,
                                                bool enforceQuota ) {

        _checkQuota( enforceQuota, fileNo - 1 );

        massert( 10358, "bad new extent size", size >= minSize() && size <= maxSize() );

        DiskLoc loc = f->allocExtentArea( txn, size );
        loc.assertOk();

        Extent *e = getExtent( loc, false );
        verify( e );

        *txn->recoveryUnit()->writing(&e->magic) = Extent::extentSignature;
        *txn->recoveryUnit()->writing(&e->myLoc) = loc;
        *txn->recoveryUnit()->writing(&e->length) = size;

        return loc;
    }


    DiskLoc MmapV1ExtentManager::_createExtent( OperationContext* txn,
                                                int size,
                                                bool enforceQuota ) {
        size = quantizeExtentSize( size );

        if ( size > maxSize() )
            size = maxSize();

        verify( size < DataFile::maxSize() );

        for ( int i = numFiles() - 1; i >= 0; i-- ) {
            DataFile* f = getFile( txn, i );
            if ( f->getHeader()->unusedLength >= size ) {
                return _createExtentInFile( txn, i, f, size, enforceQuota );
            }
        }

        _checkQuota( enforceQuota, numFiles() );

        // no space in an existing file
        // allocate files until we either get one big enough or hit maxSize
        for ( int i = 0; i < 8; i++ ) {
            DataFile* f = _addAFile( txn, size, false );

            if ( f->getHeader()->unusedLength >= size ) {
                return _createExtentInFile( txn, numFiles() - 1, f, size, enforceQuota );
            }

        }

        // callers don't check for null return code, so assert
        msgasserted(14810, "couldn't allocate space for a new extent" );
    }

    DiskLoc MmapV1ExtentManager::_allocFromFreeList( OperationContext* txn,
                                               int approxSize,
                                               bool capped ) {
        // setup extent constraints

        int low, high;
        if ( capped ) {
            // be strict about the size
            low = approxSize;
            if ( low > 2048 ) low -= 256;
            high = (int) (approxSize * 1.05) + 256;
        }
        else {
            low = (int) (approxSize * 0.8);
            high = (int) (approxSize * 1.4);
        }
        if ( high <= 0 ) {
            // overflowed
            high = max(approxSize, maxSize());
        }
        if ( high <= minSize() ) {
            // the minimum extent size is 4097
            high = minSize() + 1;
        }

        // scan free list looking for something suitable

        int n = 0;
        Extent *best = 0;
        int bestDiff = 0x7fffffff;
        {
            Timer t;
            DiskLoc L = _getFreeListStart();
            while( !L.isNull() ) {
                Extent* e = getExtent( L );
                if ( e->length >= low && e->length <= high ) {
                    int diff = abs(e->length - approxSize);
                    if ( diff < bestDiff ) {
                        bestDiff = diff;
                        best = e;
                        if ( ((double) diff) / approxSize < 0.1 ) {
                            // close enough
                            break;
                        }
                        if ( t.seconds() >= 2 ) {
                            // have spent lots of time in write lock, and we are in [low,high], so close enough
                            // could come into play if extent freelist is very long
                            break;
                        }
                    }
                    else {
                        OCCASIONALLY {
                            if ( high < 64 * 1024 && t.seconds() >= 2 ) {
                                // be less picky if it is taking a long time
                                high = 64 * 1024;
                            }
                        }
                    }
                }
                L = e->xnext;
                ++n;
            }
            if ( t.seconds() >= 10 ) {
                log() << "warning: slow scan in allocFromFreeList (in write lock)" << endl;
            }
        }

        if ( n > 128 ) { LOG( n < 512 ? 1 : 0 ) << "warning: newExtent " << n << " scanned\n"; }

        if ( !best )
            return DiskLoc();

        // remove from the free list
        if ( !best->xprev.isNull() )
            *txn->recoveryUnit()->writing(&getExtent( best->xprev )->xnext) = best->xnext;
        if ( !best->xnext.isNull() )
            *txn->recoveryUnit()->writing(&getExtent( best->xnext )->xprev) = best->xprev;
        if ( _getFreeListStart() == best->myLoc )
            _setFreeListStart( txn, best->xnext );
        if ( _getFreeListEnd() == best->myLoc )
            _setFreeListEnd( txn, best->xprev );

        return best->myLoc;
    }

    DiskLoc MmapV1ExtentManager::allocateExtent( OperationContext* txn,
                                           bool capped,
                                           int size,
                                           bool enforceQuota ) {

        bool fromFreeList = true;
        DiskLoc eloc = _allocFromFreeList( txn, size, capped );
        if ( eloc.isNull() ) {
            fromFreeList = false;
            eloc = _createExtent( txn, size, enforceQuota );
        }

        invariant( !eloc.isNull() );
        invariant( eloc.isValid() );

        LOG(1) << "MmapV1ExtentManager::allocateExtent"
               << " desiredSize:" << size
               << " fromFreeList: " << fromFreeList
               << " eloc: " << eloc;

        return eloc;
    }

    void MmapV1ExtentManager::freeExtent(OperationContext* txn, DiskLoc firstExt ) {
        Extent* e = getExtent( firstExt );
        txn->recoveryUnit()->writing( &e->xnext )->Null();
        txn->recoveryUnit()->writing( &e->xprev )->Null();
        txn->recoveryUnit()->writing( &e->firstRecord )->Null();
        txn->recoveryUnit()->writing( &e->lastRecord )->Null();


        if( _getFreeListStart().isNull() ) {
            _setFreeListStart( txn, firstExt );
            _setFreeListEnd( txn, firstExt );
        }
        else {
            DiskLoc a = _getFreeListStart();
            invariant( getExtent( a )->xprev.isNull() );
            *txn->recoveryUnit()->writing( &getExtent( a )->xprev ) = firstExt;
            *txn->recoveryUnit()->writing( &getExtent( firstExt )->xnext ) = a;
            _setFreeListStart( txn, firstExt );
        }

    }

    void MmapV1ExtentManager::freeExtents(OperationContext* txn, DiskLoc firstExt, DiskLoc lastExt) {

        if ( firstExt.isNull() && lastExt.isNull() )
            return;

        {
            verify( !firstExt.isNull() && !lastExt.isNull() );
            Extent *f = getExtent( firstExt );
            Extent *l = getExtent( lastExt );
            verify( f->xprev.isNull() );
            verify( l->xnext.isNull() );
            verify( f==l || !f->xnext.isNull() );
            verify( f==l || !l->xprev.isNull() );
        }

        if( _getFreeListStart().isNull() ) {
            _setFreeListStart( txn, firstExt );
            _setFreeListEnd( txn, lastExt );
        }
        else {
            DiskLoc a = _getFreeListStart();
            invariant( getExtent( a )->xprev.isNull() );
            *txn->recoveryUnit()->writing( &getExtent( a )->xprev ) = lastExt;
            *txn->recoveryUnit()->writing( &getExtent( lastExt )->xnext ) = a;
            _setFreeListStart( txn, firstExt );
        }

    }

    DiskLoc MmapV1ExtentManager::_getFreeListStart() const {
        if ( _files.empty() )
            return DiskLoc();
        const DataFile* file = _getOpenFile(0);
        return file->header()->freeListStart;
    }

    DiskLoc MmapV1ExtentManager::_getFreeListEnd() const {
        if ( _files.empty() )
            return DiskLoc();
        const DataFile* file = _getOpenFile(0);
        return file->header()->freeListEnd;
    }

    void MmapV1ExtentManager::_setFreeListStart( OperationContext* txn, DiskLoc loc ) {
        invariant( !_files.empty() );
        DataFile* file = _files[0];
        *txn->recoveryUnit()->writing( &file->header()->freeListStart ) = loc;
    }

    void MmapV1ExtentManager::_setFreeListEnd( OperationContext* txn, DiskLoc loc ) {
        invariant( !_files.empty() );
        DataFile* file = _files[0];
        *txn->recoveryUnit()->writing( &file->header()->freeListEnd ) = loc;
    }

    void MmapV1ExtentManager::freeListStats( int* numExtents, int64_t* totalFreeSize ) const {
        invariant( numExtents );
        invariant( totalFreeSize );

        *numExtents = 0;
        *totalFreeSize = 0;

        DiskLoc a = _getFreeListStart();
        while( !a.isNull() ) {
            Extent *e = getExtent( a );
            (*numExtents)++;
            (*totalFreeSize) += e->length;
            a = e->xnext;
        }

    }

    void MmapV1ExtentManager::printFreeList() const {
        log() << "dump freelist " << _dbname << endl;

        DiskLoc a = _getFreeListStart();
        while( !a.isNull() ) {
            Extent *e = getExtent( a );
            log() << "  extent " << a.toString()
                  << " len:" << e->length
                  << " prev:" << e->xprev.toString() << endl;
            a = e->xnext;
        }

        log() << "end freelist" << endl;
    }

    namespace {
        class CacheHintMadvise : public ExtentManager::CacheHint {
        public:
            CacheHintMadvise(void *p, unsigned len, MAdvise::Advice a)
                : _advice( p, len, a ) {
            }
        private:
            MAdvise _advice;
        };
    }

    ExtentManager::CacheHint* MmapV1ExtentManager::cacheHint( const DiskLoc& extentLoc,
                                                              const ExtentManager::HintType& hint ) {
        invariant ( hint == Sequential );
        Extent* e = getExtent( extentLoc );
        return new CacheHintMadvise( reinterpret_cast<void*>( e ),
                                     e->length,
                                     MAdvise::Sequential );
    }

    void MmapV1ExtentManager::getFileFormat( OperationContext* txn, int* major, int* minor ) const {
        if ( numFiles() == 0 )
            return;
        const DataFile* df = _getOpenFile( 0 );
        *major = df->getHeader()->version;
        *minor = df->getHeader()->versionMinor;

        if ( *major <= 0 || *major >= 100 ||
             *minor <= 0 || *minor >= 100 ) {
            error() << "corrupt pdfile version? major: " << *major << " minor: " << *minor;
            fassertFailed( 14026 );
        }
    }
}
