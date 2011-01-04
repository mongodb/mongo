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

    void MongoMMF::remapThePrivateView() {
        assert( cmdLine.dur && !testIntent );
        privateViews.remove(_view_private);
        _view_private = remapPrivateView(_view_private);
        privateViews.add(_view_private, this);
    }

    void* MongoMMF::getView() {
        if( testIntent )
            return _view_readonly;
        return _view_private;
    }

    /** register view. threadsafe */
    void PointerToMMF::add(void *view, MongoMMF *f) {
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
    MongoMMF* PointerToMMF::_find(void *p, /*out*/ size_t& ofs) {
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
        return _find(p, ofs);
    }

    PointerToMMF privateViews;
    static PointerToMMF ourReadViews; /// _TESTINTENT (testIntent) build use only (other than existance)

    /*static*/ void* MongoMMF::switchToPrivateView(void *readonly_ptr) {
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
    }

    /* switch to _view_write.  normally, this is a bad idea since your changes will not
       show up in _view_private if there have been changes there; thus the leading underscore
       as a tad of a "warning".  but useful when done with some care, such as during
       initialization.
    */
    /*static*/ void* MongoMMF::_switchToWritableView(void *p) {
        RARELY log() << "todo dur not done switchtowritable" << endl;
        if( debug )
            return switchToPrivateView(p);
        return p;
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
        if( cmdLine.dur && !testIntent && _view_write && !preExisting ) {
            getDur().createdFile(fname, len);
        }
        return finishOpening();
    }

    bool MongoMMF::finishOpening() {
        if( _view_write ) {
            if( cmdLine.dur ) {
                if( testIntent ) {
                    _view_private = _view_write;
                    _view_readonly = MemoryMappedFile::createReadOnlyMap();
                    ourReadViews.add(_view_readonly, this);
                }
                else {
                    _view_private = createPrivateMap();
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
        _view_write = _view_private = _view_readonly = 0;
    }

    MongoMMF::~MongoMMF() {
        close();
    }

    namespace dur {
        void closingFileNotification();
    }

    /*virtual*/ void MongoMMF::close() {
        {
            if( !testIntent && cmdLine.dur && _view_write/*actually was opened*/ ) {
                if( debug )
                    log() << "closingFileNotication:" << filename() << endl;
                dur::closingFileNotification();
            }
            privateViews.remove(_view_private);
            if( debug ) {
                ourReadViews.remove(_view_readonly);
            }
        }
        _view_write = _view_private = _view_readonly = 0;
        MemoryMappedFile::close();
    }

}
