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
*/

#include "mongo/pch.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/client.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/storage/data_file.h"
#include "mongo/db/storage/extent_manager.h"

// XXX-erh
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

    // todo: this is called a lot. streamline the common case
    DataFile* ExtentManager::getFile( int n, int sizeNeeded , bool preallocateOnly) {
        verify(this);
        DEV Lock::assertAtLeastReadLocked( _dbname );

        if ( n < 0 || n >= DiskLoc::MaxFiles ) {
            log() << "getFile(): n=" << n << endl;
            massert( 10295 , "getFile(): bad file number value (corrupt db?): run repair", false);
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
                    log() << "error: getFile() called in a read lock, yet file to return is not yet open" << endl;
                    log() << "       getFile(" << n << ") _files.size:" <<_files.size() << ' ' << fileName(n).string() << endl;
                    log() << "       context ns: " << cc().ns() << endl;
                    verify(false);
                }
                _files.push_back(0);
            }
            p = _files[n];
        }
        if ( p == 0 ) {
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

    Record* ExtentManager::recordFor( const DiskLoc& loc ) {
        loc.assertOk();
        DataFile* df = getFile( loc.a() );

        int ofs = loc.getOfs();
        if ( ofs < DataFileHeader::HeaderSize ) {
            df->badOfs(ofs); // will uassert - external call to keep out of the normal code path
        }

        return reinterpret_cast<Record*>( df->p() + ofs );
    }

    Extent* ExtentManager::extentFor( const DiskLoc& loc ) {
        Record* record = recordFor( loc );
        DiskLoc extentLoc( loc.a(), record->extentOfs() );
        return getExtent( extentLoc );
    }

    Extent* ExtentManager::getExtent( const DiskLoc& loc, bool doSanityCheck ) {
        loc.assertOk();
        Extent* e = reinterpret_cast<Extent*>( getFile( loc.a() )->p() + loc.getOfs() );
        if ( doSanityCheck )
            e->assertOk();
        return e;
    }


    DiskLoc ExtentManager::getNextRecordInExtent( const DiskLoc& loc ) {
        int nextOffset = recordFor( loc )->nextOfs();

        if ( nextOffset == DiskLoc::NullOfs )
            return DiskLoc();

        fassert( 16967, abs(nextOffset) >= 8 ); // defensive
        return DiskLoc( loc.a(), nextOffset );
    }

    DiskLoc ExtentManager::getNextRecord( const DiskLoc& loc ) {
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

    DiskLoc ExtentManager::getPrevRecordInExtent( const DiskLoc& loc ) {
        int prevOffset = recordFor( loc )->prevOfs();

        if ( prevOffset == DiskLoc::NullOfs )
            return DiskLoc();

        fassert( 16968, abs(prevOffset) >= 8 ); // defensive
        return DiskLoc( loc.a(), prevOffset );
    }

    DiskLoc ExtentManager::getPrevRecord( const DiskLoc& loc ) {
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
        return e->firstRecord;
    }

    int ExtentManager::quantizeExtentSize( int size ) {
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

    bool fileIndexExceedsQuota( const char *ns, int fileIndex ) {
        return
            cmdLine.quota &&
            fileIndex >= cmdLine.quotaFiles &&
            // we don't enforce the quota on "special" namespaces as that could lead to problems -- e.g.
            // rejecting an index insert after inserting the main record.
            !NamespaceString::special( ns ) &&
            nsToDatabaseSubstring( ns ) != "local";
    }

    // XXX-ERH
    void addNewExtentToNamespace(const char *ns, Extent *e, DiskLoc eloc, DiskLoc emptyLoc, bool capped);

    Extent* ExtentManager::_createExtentInFile( int fileNo, DataFile* f,
                                                const char* ns, int size, bool newCapped,
                                                bool enforceQuota ) {

        size = ExtentManager::quantizeExtentSize( size );

        if ( enforceQuota ) {
            if ( fileIndexExceedsQuota( ns, fileNo - 1 ) ) {
                if ( cc().hasWrittenThisPass() ) {
                    warning() << "quota exceeded, but can't assert, going over quota for: " << ns << endl;
                }
                else {
                    uasserted(12501, "quota exceeded");
                }
            }
        }

        massert( 10358, "bad new extent size", size >= Extent::minSize() && size <= Extent::maxSize() );

        DiskLoc loc = f->allocExtentArea( size );
        loc.assertOk();

        Extent *e = getExtent( loc, false );
        verify( e );

        DiskLoc emptyLoc = getDur().writing(e)->init(ns, size, fileNo, loc.getOfs(), newCapped);

        addNewExtentToNamespace(ns, e, loc, emptyLoc, newCapped);

        return e;
    }


    Extent* ExtentManager::createExtent(const char *ns, int size, bool newCapped, bool enforceQuota ) {
        size = quantizeExtentSize( size );

        for ( int i = numFiles() - 1; i >= 0; i-- ) {
            DataFile* f = getFile( i );
            if ( f->getHeader()->unusedLength >= size ) {
                return _createExtentInFile( i, f, ns, size, newCapped, enforceQuota );
            }
        }

        // no space in an existing file
        // allocate files until we either get one big enough or hit maxSize
        for ( int i = 0; i < 8; i++ ) {
            DataFile* f = addAFile( size, false );

            if ( f->getHeader()->unusedLength >= size ||
                 f->getHeader()->fileLength >= DataFile::maxSize() ) {
                return _createExtentInFile( numFiles() - 1, f, ns, size, newCapped, enforceQuota );
            }

        }

        // callers don't check for null return code, so assert
        msgasserted(14810, "couldn't allocate space for a new extent" );
    }

}
