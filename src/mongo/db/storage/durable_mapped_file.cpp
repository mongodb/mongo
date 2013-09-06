// durable_mapped_file.cpp

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

/* this module adds some of our layers atop memory mapped files - specifically our handling of private views & such
   if you don't care about journaling/durability (temp sort files & such) use MemoryMappedFile class, not this.
*/

#include "mongo/pch.h"

#include "mongo/db/storage/durable_mapped_file.h"

#include "mongo/db/cmdline.h"

#include "mongo/db/d_concurrency.h"
#include "mongo/db/dur.h"
#include "mongo/db/dur_journalformat.h"
#include "mongo/db/memconcept.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongoutils;

namespace mongo {

    void DurableMappedFile::remapThePrivateView() {
        verify( cmdLine.dur );

        // todo 1.9 : it turns out we require that we always remap to the same address.
        // so the remove / add isn't necessary and can be removed?
        void *old = _view_private;
        //privateViews.remove(_view_private);        
        _view_private = remapPrivateView(_view_private);
        //privateViews.add(_view_private, this);
        fassert( 16112, _view_private == old );
    }

    /** register view. threadsafe */
    void PointerToDurableMappedFile::add(void *view, DurableMappedFile *f) {
        verify(view);
        verify(f);
        mutex::scoped_lock lk(_m);
        _views.insert( pair<void*,DurableMappedFile*>(view,f) );
    }

    /** de-register view. threadsafe */
    void PointerToDurableMappedFile::remove(void *view) {
        if( view ) {
            mutex::scoped_lock lk(_m);
            _views.erase(view);
        }
    }

    PointerToDurableMappedFile::PointerToDurableMappedFile() : _m("PointerToDurableMappedFile") {
#if defined(SIZE_MAX)
        size_t max = SIZE_MAX;
#else
        size_t max = ~((size_t)0);
#endif
        verify( max > (size_t) this ); // just checking that no one redef'd SIZE_MAX and that it is sane

        // this way we don't need any boundary checking in _find()
        _views.insert( pair<void*,DurableMappedFile*>((void*)0,(DurableMappedFile*)0) );
        _views.insert( pair<void*,DurableMappedFile*>((void*)max,(DurableMappedFile*)0) );
    }

    /** underscore version of find is for when you are already locked
        @param ofs out return our offset in the view
        @return the DurableMappedFile to which this pointer belongs
    */
    DurableMappedFile* PointerToDurableMappedFile::find_inlock(void *p, /*out*/ size_t& ofs) {
        //
        // .................memory..........................
        //    v1       p                      v2
        //    [--------------------]          [-------]
        //
        // e.g., _find(p) == v1
        //
        const pair<void*,DurableMappedFile*> x = *(--_views.upper_bound(p));
        DurableMappedFile *mmf = x.second;
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
        @return the DurableMappedFile to which this pointer belongs. null if not found.
    */
    DurableMappedFile* PointerToDurableMappedFile::find(void *p, /*out*/ size_t& ofs) {
        mutex::scoped_lock lk(_m);
        return find_inlock(p, ofs);
    }

    PointerToDurableMappedFile privateViews;

    extern string dbpath;

    // here so that it is precomputed...
    void DurableMappedFile::setPath(const std::string& f) {
        string suffix;
        string prefix;
        bool ok = str::rSplitOn(f, '.', prefix, suffix);
        uassert(13520, str::stream() << "DurableMappedFile only supports filenames in a certain format " << f, ok);
        if( suffix == "ns" )
            _fileSuffixNo = dur::JEntry::DotNsSuffix;
        else
            _fileSuffixNo = (int) str::toUnsigned(suffix);

        _p = RelativePath::fromFullPath(prefix);
    }

    bool DurableMappedFile::open(const std::string& fname, bool sequentialHint) {
        LOG(3) << "mmf open " << fname << endl;
        setPath(fname);
        _view_write = mapWithOptions(fname.c_str(), sequentialHint ? SEQUENTIAL : 0);
        return finishOpening();
    }

    bool DurableMappedFile::create(const std::string& fname, unsigned long long& len, bool sequentialHint) {
        LOG(3) << "mmf create " << fname << endl;
        setPath(fname);
        _view_write = map(fname.c_str(), len, sequentialHint ? SEQUENTIAL : 0);
        return finishOpening();
    }

    bool DurableMappedFile::finishOpening() {
        LOG(3) << "mmf finishOpening " << (void*) _view_write << ' ' << filename() << " len:" << length() << endl;
        if( _view_write ) {
            if( cmdLine.dur ) {
                _view_private = createPrivateMap();
                if( _view_private == 0 ) {
                    msgasserted(13636, str::stream() << "file " << filename() << " open/create failed in createPrivateMap (look in log for more information)");
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

    DurableMappedFile::DurableMappedFile() : _willNeedRemap(false) {
        _view_write = _view_private = 0;
    }

    DurableMappedFile::~DurableMappedFile() {
        try { 
            close();
        }
        catch(...) { error() << "exception in ~DurableMappedFile" << endl; }
    }

    namespace dur {
        void closingFileNotification();
    }

    /*virtual*/ void DurableMappedFile::close() {
        LOG(3) << "mmf close " << filename() << endl;

        if( view_write() /*actually was opened*/ ) {
            if( cmdLine.dur ) {
                dur::closingFileNotification();
            }
            /* todo: is it ok to close files if we are not globally locked exclusively?
                     probably, but need to review. also note the lock assert below is
                     rather vague and not checking if the right database is locked 
            */
            if( !Lock::somethingWriteLocked() ) { 
                verify( inShutdown() );
                DEV { 
                    log() << "is it really ok to close a mongommf outside a write lock? file:" << filename() << endl;
                }
            }
        }

        LockMongoFilesExclusive lk;
        privateViews.remove(_view_private);
        memconcept::invalidate(_view_private);
        _view_write = _view_private = 0;
        MemoryMappedFile::close();
    }

}
