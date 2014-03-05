// extent_manager.cpp

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

#include <boost/filesystem/operations.hpp>

#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/dur.h"
#include "mongo/db/storage/data_file.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/catalog/namespace_details.h"

#include "mongo/db/pdfile.h"

namespace mongo {

    ExtentManager::ExtentManager( const StringData& dbname,
                                  const StringData& path,
                                  bool directoryPerDB )
        : _dbname( dbname.toString() ),
          _path( path.toString() ),
          _directoryPerDB( directoryPerDB ) {
    }

    ExtentManager::~ExtentManager() {
        reset();
    }

    void ExtentManager::reset() {
        for ( size_t i = 0; i < _files.size(); i++ ) {
            delete _files[i];
        }
        _files.clear();
    }

    boost::filesystem::path ExtentManager::fileName( int n ) const {
        stringstream ss;
        ss << _dbname << '.' << n;
        boost::filesystem::path fullName( _path );
        if ( _directoryPerDB )
            fullName /= _dbname;
        fullName /= ss.str();
        return fullName;
    }


    Status ExtentManager::init() {
        verify( _files.size() == 0 );

        for ( int n = 0; n < DiskLoc::MaxFiles; n++ ) {
            boost::filesystem::path fullName = fileName( n );
            if ( !boost::filesystem::exists( fullName ) )
                break;

            string fullNameString = fullName.string();

            auto_ptr<DataFile> df( new DataFile(n) );

            Status s = df->openExisting( fullNameString.c_str() );
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

    const DataFile* ExtentManager::_getOpenFile( int n ) const {
        verify(this);
        DEV Lock::assertAtLeastReadLocked( _dbname );
        if ( n < 0 || n >= static_cast<int>(_files.size()) )
            log() << "uh oh: " << n;
        verify( n >= 0 && n < static_cast<int>(_files.size()) );
        return _files[n];
    }


    // todo: this is called a lot. streamline the common case
    DataFile* ExtentManager::getFile( int n, int sizeNeeded , bool preallocateOnly) {
        verify(this);
        DEV Lock::assertAtLeastReadLocked( _dbname );

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
                if( !Lock::isWriteLocked(_dbname) ) {
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
            DEV Lock::assertWriteLocked( _dbname );
            boost::filesystem::path fullName = fileName( n );
            string fullNameString = fullName.string();
            p = new DataFile(n);
            int minSize = 0;
            if ( n != 0 && _files[ n - 1 ] )
                minSize = _files[ n - 1 ]->getHeader()->fileLength;
            if ( sizeNeeded + DataFileHeader::HeaderSize > minSize )
                minSize = sizeNeeded + DataFileHeader::HeaderSize;
            try {
                p->open( fullNameString.c_str(), minSize, preallocateOnly );
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

    DataFile* ExtentManager::addAFile( int sizeNeeded, bool preallocateNextFile ) {
        DEV Lock::assertWriteLocked( _dbname );
        int n = (int) _files.size();
        DataFile *ret = getFile( n, sizeNeeded );
        if ( preallocateNextFile )
            preallocateAFile();
        return ret;
    }

    size_t ExtentManager::numFiles() const {
        DEV Lock::assertAtLeastReadLocked( _dbname );
        return _files.size();
    }

    long long ExtentManager::fileSize() const {
        long long size=0;
        for ( int n = 0; boost::filesystem::exists( fileName(n) ); n++)
            size += boost::filesystem::file_size( fileName(n) );
        return size;
    }

    void ExtentManager::flushFiles( bool sync ) {
        DEV Lock::assertAtLeastReadLocked( _dbname );
        for( vector<DataFile*>::iterator i = _files.begin(); i != _files.end(); i++ ) {
            DataFile *f = *i;
            f->flush(sync);
        }
    }

    Record* ExtentManager::recordFor( const DiskLoc& loc ) const {
        loc.assertOk();
        const DataFile* df = _getOpenFile( loc.a() );

        int ofs = loc.getOfs();
        if ( ofs < DataFileHeader::HeaderSize ) {
            df->badOfs(ofs); // will uassert - external call to keep out of the normal code path
        }

        return reinterpret_cast<Record*>( df->p() + ofs );
    }

    DiskLoc ExtentManager::extentLocFor( const DiskLoc& loc ) const {
        Record* record = recordFor( loc );
        return DiskLoc( loc.a(), record->extentOfs() );
    }

    Extent* ExtentManager::extentFor( const DiskLoc& loc ) const {
        DiskLoc extentLoc = extentLocFor( loc );
        return getExtent( extentLoc );
    }

    Extent* ExtentManager::getExtent( const DiskLoc& loc, bool doSanityCheck ) const {
        loc.assertOk();
        Extent* e = reinterpret_cast<Extent*>( _getOpenFile( loc.a() )->p() + loc.getOfs() );
        if ( doSanityCheck )
            e->assertOk();
        return e;
    }


    DiskLoc ExtentManager::getNextRecordInExtent( const DiskLoc& loc ) const {
        int nextOffset = recordFor( loc )->nextOfs();

        if ( nextOffset == DiskLoc::NullOfs )
            return DiskLoc();

        fassert( 16967, abs(nextOffset) >= 8 ); // defensive
        return DiskLoc( loc.a(), nextOffset );
    }

    DiskLoc ExtentManager::getNextRecord( const DiskLoc& loc ) const {
        DiskLoc next = getNextRecordInExtent( loc );
        if ( !next.isNull() )
            return next;

        // now traverse extents

        Extent *e = extentFor(loc);
        while ( 1 ) {
            if ( e->xnext.isNull() )
                return DiskLoc(); // end of collection
            e = e->xnext.ext();
            if ( !e->firstRecord.isNull() )
                break;
            // entire extent could be empty, keep looking
        }
        return e->firstRecord;
    }

    DiskLoc ExtentManager::getPrevRecordInExtent( const DiskLoc& loc ) const {
        int prevOffset = recordFor( loc )->prevOfs();

        if ( prevOffset == DiskLoc::NullOfs )
            return DiskLoc();

        fassert( 16968, abs(prevOffset) >= 8 ); // defensive
        return DiskLoc( loc.a(), prevOffset );
    }

    DiskLoc ExtentManager::getPrevRecord( const DiskLoc& loc ) const {
        DiskLoc prev = getPrevRecordInExtent( loc );
        if ( !prev.isNull() )
            return prev;

        // now traverse extents

        Extent *e = extentFor(loc);
        while ( 1 ) {
            if ( e->xprev.isNull() )
                return DiskLoc(); // end of collection
            e = e->xprev.ext();
            if ( !e->firstRecord.isNull() )
                break;
            // entire extent could be empty, keep looking
        }
        return e->lastRecord;
    }

    Extent* ExtentManager::getNextExtent( Extent* e ) const {
        if ( e->xnext.isNull() )
            return NULL;
        return getExtent( e->xnext );
    }

    Extent* ExtentManager::getPrevExtent( Extent* e ) const {
        if ( e->xprev.isNull() )
            return NULL;
        return getExtent( e->xprev );
    }

    int ExtentManager::quantizeExtentSize( int size ) {

        if ( size == Extent::maxSize() ) {
            // no point doing quantizing for the entire file
            return size;
        }

        verify( size <= Extent::maxSize() );

        // make sizes align with VM page size
        int newSize = (size + 0xfff) & 0xfffff000;

        if ( newSize > Extent::maxSize() ) {
            return Extent::maxSize();
        }

        if ( newSize < Extent::minSize() ) {
            return Extent::minSize();
        }

        return newSize;
    }

    void _quotaExceeded() {
        uasserted(12501, "quota exceeded");
    }

    DiskLoc ExtentManager::_createExtentInFile( int fileNo, DataFile* f,
                                                int size, int maxFileNoForQuota ) {

        size = ExtentManager::quantizeExtentSize( size );

        if ( maxFileNoForQuota > 0 && fileNo - 1 >= maxFileNoForQuota ) {
            if ( cc().hasWrittenSinceCheckpoint() ) {
                warning() << "quota exceeded, but can't assert" << endl;
            }
            else {
                _quotaExceeded();
            }
        }

        massert( 10358, "bad new extent size", size >= Extent::minSize() && size <= Extent::maxSize() );

        DiskLoc loc = f->allocExtentArea( size );
        loc.assertOk();

        Extent *e = getExtent( loc, false );
        verify( e );

        getDur().writing(e)->init("", size, fileNo, loc.getOfs(), false);

        return loc;
    }


    DiskLoc ExtentManager::createExtent( int size, int maxFileNoForQuota ) {
        size = quantizeExtentSize( size );

        if ( size > Extent::maxSize() )
            size = Extent::maxSize();

        verify( size < DataFile::maxSize() );

        for ( int i = numFiles() - 1; i >= 0; i-- ) {
            DataFile* f = getFile( i );
            if ( f->getHeader()->unusedLength >= size ) {
                return _createExtentInFile( i, f, size, maxFileNoForQuota );
            }
        }

        if ( maxFileNoForQuota > 0 &&
             static_cast<int>( numFiles() ) >= maxFileNoForQuota &&
             !cc().hasWrittenSinceCheckpoint() ) {
            _quotaExceeded();
        }


        // no space in an existing file
        // allocate files until we either get one big enough or hit maxSize
        for ( int i = 0; i < 8; i++ ) {
            DataFile* f = addAFile( size, false );

            if ( f->getHeader()->unusedLength >= size ) {
                return _createExtentInFile( numFiles() - 1, f, size, maxFileNoForQuota );
            }

        }

        // callers don't check for null return code, so assert
        msgasserted(14810, "couldn't allocate space for a new extent" );
    }

    DiskLoc ExtentManager::allocFromFreeList( int approxSize, bool capped ) {
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
            high = max(approxSize, Extent::maxSize());
        }
        if ( high <= Extent::minSize() ) {
            // the minimum extent size is 4097
            high = Extent::minSize() + 1;
        }

        // scan free list looking for something suitable

        int n = 0;
        Extent *best = 0;
        int bestDiff = 0x7fffffff;
        {
            Timer t;
            DiskLoc L = _getFreeListStart();
            while( !L.isNull() ) {
                Extent * e = L.ext();
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
            getExtent( best->xprev )->xnext.writing() = best->xnext;
        if ( !best->xnext.isNull() )
            getExtent( best->xnext )->xprev.writing() = best->xprev;
        if ( _getFreeListStart() == best->myLoc )
            _setFreeListStart( best->xnext );
        if ( _getFreeListEnd() == best->myLoc )
            _setFreeListEnd( best->xprev );

        return best->myLoc;
    }


    Extent* ExtentManager::increaseStorageSize( const string& ns,
                                                NamespaceDetails* details,
                                                int size,
                                                int quotaMax ) {

        bool fromFreeList = true;
        DiskLoc eloc = allocFromFreeList( size, details->isCapped() );
        if ( eloc.isNull() ) {
            fromFreeList = false;
            eloc = createExtent( size, quotaMax );
        }

        verify( !eloc.isNull() );
        verify( eloc.isValid() );

        LOG(1) << "ExtentManager::increaseStorageSize"
               << " ns:" << ns
               << " desiredSize:" << size
               << " fromFreeList: " << fromFreeList
               << " eloc: " << eloc;

        Extent *e = getExtent( eloc, false );
        verify( e );

        DiskLoc emptyLoc = getDur().writing(e)->reuse( ns,
                                                       details->isCapped() );

        if ( details->lastExtent().isNull() ) {
            verify( details->firstExtent().isNull() );
            details->setFirstExtent( eloc );
            details->setLastExtent( eloc );
            getDur().writingDiskLoc( details->capExtent() ) = eloc;
            verify( e->xprev.isNull() );
            verify( e->xnext.isNull() );
        }
        else {
            verify( !details->firstExtent().isNull() );
            getDur().writingDiskLoc(e->xprev) = details->lastExtent();
            getDur().writingDiskLoc(details->lastExtent().ext()->xnext) = eloc;
            details->setLastExtent( eloc );
        }

        details->setLastExtentSize( e->length );

        details->addDeletedRec(emptyLoc.drec(), emptyLoc);

        return e;
    }

    void ExtentManager::freeExtents(DiskLoc firstExt, DiskLoc lastExt) {

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
            _setFreeListStart( firstExt );
            _setFreeListEnd( lastExt );
        }
        else {
            DiskLoc a = _getFreeListStart();
            invariant( getExtent( a )->xprev.isNull() );
            getDur().writingDiskLoc( getExtent( a )->xprev ) = lastExt;
            getDur().writingDiskLoc( getExtent( lastExt )->xnext ) = a;
            _setFreeListStart( firstExt );
        }

    }

    DiskLoc ExtentManager::_getFreeListStart() const {
        if ( _files.empty() )
            return DiskLoc();
        const DataFile* file = _getOpenFile(0);
        return file->header()->freeListStart;
    }

    DiskLoc ExtentManager::_getFreeListEnd() const {
        if ( _files.empty() )
            return DiskLoc();
        const DataFile* file = _getOpenFile(0);
        return file->header()->freeListEnd;
    }

    void ExtentManager::_setFreeListStart( DiskLoc loc ) {
        invariant( !_files.empty() );
        DataFile* file = _files[0];
        getDur().writingDiskLoc( file->header()->freeListStart ) = loc;
    }

    void ExtentManager::_setFreeListEnd( DiskLoc loc ) {
        invariant( !_files.empty() );
        DataFile* file = _files[0];
        getDur().writingDiskLoc( file->header()->freeListEnd ) = loc;
    }

    void ExtentManager::freeListStats( int* numExtents, int64_t* totalFreeSize ) const {
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

    void ExtentManager::printFreeList() const {
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


}
