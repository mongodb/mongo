// @file mongommf.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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

/* this module adds some of our layers atop memory mapped files - specifically our handling of private views & such
   if you don't care about journaling/durability (temp sort files & such) use MemoryMappedFile class, not this.
*/

#include "pch.h"
#include "cmdline.h"
#include "mongommf.h"
#include "dur.h"
#include "dur_journalformat.h"
#include "../util/mongoutils/str.h"

using namespace mongoutils;

namespace mongo {

#if defined(_WIN32)
    extern mutex mapViewMutex;

    __declspec(noinline) void makeChunkWritable(size_t chunkno) { 
        scoped_lock lk(mapViewMutex);

        if( writable.get(chunkno) )
            return;

        size_t loc = chunkno * MemoryMappedFile::ChunkSize;
        void *Loc = (void*) loc;
        size_t ofs;
        MongoMMF *mmf = privateViews.find( (void *) (loc), ofs );
        MemoryMappedFile *f = (MemoryMappedFile*) mmf;
        assert(f);

        size_t len = MemoryMappedFile::ChunkSize;
        assert( mmf->getView() <= Loc );
        if( ofs + len > f->length() ) {
            // at the very end of the map
            len = f->length() - ofs;
        }
        else { 
            ;
        }

        // todo: check this goes away on remap
        DWORD old;
        bool ok = VirtualProtect(Loc, len, PAGE_WRITECOPY, &old);
        if( !ok ) {
            DWORD e = GetLastError();
            cout << "virtualprotect " << Loc << ' ' << len << ' ' << e << endl;
            assert(false);
        }

        writable.set(chunkno);
    }

    // align so that there is only one map per chunksize so our bitset works right
    void* mapaligned(HANDLE h, unsigned long long _len) {
        void *loc = 0;
        int n = 0;
        while( 1 ) { 
            n++;
            void *m = MapViewOfFileEx(h, FILE_MAP_READ, 0, 0, 0, loc);
            if( m == 0 ) {
                DWORD e = GetLastError();
                if( n == 0 ) { 
                    // if first fails, it isn't going to work
                    log() << "mapaligned errno: " << e << endl;
                    break;
                }
                if( debug && n == 1 ) { 
                    log() << "mapaligned info e:" << e << " at n=1" << endl;
                }
                if( n > 98 ) {
                    log() << "couldn't align mapped view of file len:" << _len/1024.0/1024.0 << "MB errno:" << e << endl;
                    break;
                }
                loc = (void*) (((size_t)loc)+MemoryMappedFile::ChunkSize);
                continue;
            }

            size_t x = (size_t) m;
            if( x % MemoryMappedFile::ChunkSize == 0 ) {
                void *end = (void*) (x+_len);
                DEV log() << "mapaligned " << m << '-' << end << " len:" << _len << endl;
                return m;
            }

            UnmapViewOfFile(m);
            x = ((x+MemoryMappedFile::ChunkSize-1) / MemoryMappedFile::ChunkSize) * MemoryMappedFile::ChunkSize;
            loc = (void*) x;
            if( n % 20 == 0 ) { 
                log() << "warning mapaligned n=20" << endl;
            }
            if( n > 100 ) {
                log() << "couldn't align mapped view of file len:" << _len/1024.0/1024.0 << "MB" << endl;
                break;
            }
        }
        return 0;
    }

    void* MemoryMappedFile::createPrivateMap() {
        assert( maphandle );
        scoped_lock lk(mapViewMutex);
        void *p = mapaligned(maphandle, len);
        if ( p == 0 ) {
            DWORD e = GetLastError();
            log() << "createPrivateMap failed " << filename() << " " << errnoWithDescription(e) << endl;
        }
        else {
            views.push_back(p);
        }
        return p;
    }

    void* MemoryMappedFile::remapPrivateView(void *oldPrivateAddr) {
        // the mutex is to assure we get the same address on the remap
        dbMutex.assertWriteLocked();

        scoped_lock lk(mapViewMutex);

        unmapped(oldPrivateAddr);

        bool ok = UnmapViewOfFile(oldPrivateAddr);
        assert(ok);

        // we want the new address to be the same as the old address in case things keep pointers around (as namespaceindex does).
        void *p = MapViewOfFileEx(maphandle, FILE_MAP_READ, 0, 0,
                                  /*dwNumberOfBytesToMap 0 means to eof*/0 /*len*/,
                                  oldPrivateAddr);
        
        if ( p == 0 ) {
            DWORD e = GetLastError();
            log() << "MapViewOfFileEx failed " << filename() << " " << errnoWithDescription(e) << endl;
            assert(p);
        }
        assert(p == oldPrivateAddr);
        return p;
    }
#endif

    void MongoMMF::remapThePrivateView() {
        assert( cmdLine.dur );

        // todo 1.9 : it turns out we require that we always remap to the same address.
        // so the remove / add isn't necessary and can be removed
        privateViews.remove(_view_private);
        _view_private = remapPrivateView(_view_private);
        privateViews.add(_view_private, this);
    }

    /** register view. threadsafe */
    void PointerToMMF::add(void *view, MongoMMF *f) {
        assert(view);
        assert(f);
        mutex::scoped_lock lk(_m);
        _views.insert( pair<void*,MongoMMF*>(view,f) );
    }

    /** de-register view. threadsafe */
    void PointerToMMF::remove(void *view) {
        if( view ) {
            mutex::scoped_lock lk(_m);
            _views.erase(view);
        }
    }

    PointerToMMF::PointerToMMF() : _m("PointerToMMF") {
#if defined(SIZE_MAX)
        size_t max = SIZE_MAX;
#else
        size_t max = ~((size_t)0);
#endif
        assert( max > (size_t) this ); // just checking that no one redef'd SIZE_MAX and that it is sane

        // this way we don't need any boundary checking in _find()
        _views.insert( pair<void*,MongoMMF*>((void*)0,(MongoMMF*)0) );
        _views.insert( pair<void*,MongoMMF*>((void*)max,(MongoMMF*)0) );
    }

    /** underscore version of find is for when you are already locked
        @param ofs out return our offset in the view
        @return the MongoMMF to which this pointer belongs
    */
    MongoMMF* PointerToMMF::find_inlock(void *p, /*out*/ size_t& ofs) {
        //
        // .................memory..........................
        //    v1       p                      v2
        //    [--------------------]          [-------]
        //
        // e.g., _find(p) == v1
        //
        const pair<void*,MongoMMF*> x = *(--_views.upper_bound(p));
        MongoMMF *mmf = x.second;
        if( mmf ) {
            size_t o = ((char *)p) - ((char*)x.first);
            if( o < mmf->length() ) {
                ofs = o;
                return mmf;
            }
        }
        return 0;
    }

    /** find associated MMF object for a given pointer.
        threadsafe
        @param ofs out returns offset into the view of the pointer, if found.
        @return the MongoMMF to which this pointer belongs. null if not found.
    */
    MongoMMF* PointerToMMF::find(void *p, /*out*/ size_t& ofs) {
        mutex::scoped_lock lk(_m);
        return find_inlock(p, ofs);
    }

    PointerToMMF privateViews;

    /* void* MongoMMF::switchToPrivateView(void *readonly_ptr) {
        assert( cmdLine.dur );
        assert( testIntent );

        void *p = readonly_ptr;

        {
            size_t ofs=0;
            MongoMMF *mmf = ourReadViews.find(p, ofs);
            if( mmf ) {
                void *res = ((char *)mmf->_view_private) + ofs;
                return res;
            }
        }

        {
            size_t ofs=0;
            MongoMMF *mmf = privateViews.find(p, ofs);
            if( mmf ) {
                log() << "dur: perf warning p=" << p << " is already in the writable view of " << mmf->filename() << endl;
                return p;
            }
        }

        // did you call writing() with a pointer that isn't into a datafile?
        log() << "dur error switchToPrivateView " << p << endl;
        return p;
    }*/

    /* switch to _view_write.  normally, this is a bad idea since your changes will not
       show up in _view_private if there have been changes there; thus the leading underscore
       as a tad of a "warning".  but useful when done with some care, such as during
       initialization.
    */
    void* MongoMMF::_switchToWritableView(void *p) {
        size_t ofs;
        MongoMMF *f = privateViews.find(p, ofs);
        assert( f );
        return (((char *)f->_view_write)+ofs);
    }

    extern string dbpath;

    // here so that it is precomputed...
    void MongoMMF::setPath(string f) {
        string suffix;
        string prefix;
        bool ok = str::rSplitOn(f, '.', prefix, suffix);
        uassert(13520, str::stream() << "MongoMMF only supports filenames in a certain format " << f, ok);
        if( suffix == "ns" )
            _fileSuffixNo = dur::JEntry::DotNsSuffix;
        else
            _fileSuffixNo = (int) str::toUnsigned(suffix);

        _p = RelativePath::fromFullPath(prefix);
    }

    bool MongoMMF::open(string fname, bool sequentialHint) {
        setPath(fname);
        _view_write = mapWithOptions(fname.c_str(), sequentialHint ? SEQUENTIAL : 0);
        return finishOpening();
    }

    bool MongoMMF::create(string fname, unsigned long long& len, bool sequentialHint) {
        setPath(fname);
        bool preExisting = MemoryMappedFile::exists(fname.c_str());
        _view_write = map(fname.c_str(), len, sequentialHint ? SEQUENTIAL : 0);
        if( cmdLine.dur && _view_write && !preExisting ) {
            getDur().createdFile(fname, len);
        }
        return finishOpening();
    }

    bool MongoMMF::finishOpening() {
        if( _view_write ) {
            if( cmdLine.dur ) {
                _view_private = createPrivateMap();
                if( _view_private == 0 ) {
                    massert( 13636 , "createPrivateMap failed (look in log for error)" , false );
                }
                privateViews.add(_view_private, this); // note that testIntent builds use this, even though it points to view_write then...
            }
            else {
                _view_private = _view_write;
            }
            return true;
        }
        return false;
    }

    MongoMMF::MongoMMF() : _willNeedRemap(false) {
        _view_write = _view_private = 0;
    }

    MongoMMF::~MongoMMF() {
        close();
    }

    namespace dur {
        void closingFileNotification();
    }

    /*virtual*/ void MongoMMF::close() {
        {
            if( cmdLine.dur && _view_write/*actually was opened*/ ) {
                if( debug )
                    log() << "closingFileNotication:" << filename() << endl;
                dur::closingFileNotification();
            }
            privateViews.remove(_view_private);
        }
        _view_write = _view_private = 0;
        MemoryMappedFile::close();
    }

}
