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
#include "mongommf.h"

namespace mongo {

    /* see dur.h */
    static map<void *, MongoMMF*> our_read_views;
    static mutex our_views_mutex("");
    void* MongoMMF::switchToWritableView(void *p) { 
        assert( durable );
        assert( debug );
        mutex::scoped_lock lk(our_views_mutex);
        std::map< void*, MongoMMF* >::iterator i = 
            our_read_views.upper_bound(((char *)p)+1);
        i--;

        bool ok = i != our_read_views.end();
        if( ok ) {
            MongoMMF *mmf = i->second;
            assert( mmf );

            size_t ofs = ((char *)p) - ((char*)mmf->view_readonly);

            if( ofs < mmf->length() ) { 
                return ((char *)mmf->view_private) + ofs;
            }
        }

        if( 1 ) { 
            /* temp : not using MongoMMF yet for datafiles, just .ns.  more to do... */
            log() << "TEMP TODO" << endl;
            return p;
        }

        for( std::map<void*,MongoMMF*>::iterator i = our_read_views.begin(); i != our_read_views.end(); i++ ) { 
            char *wl = (char *) i->second->view_private;
            char *wh = wl + i->second->length();
            if( p >= wl && p < wh ) { 
                log() << "dur: perf warning p=" << p << " is already in the writable view of " << i->second->filename() << endl;
                return p;
            }
        }
        log() << "switchToWritableView error " << p << endl;
        assert( false ); // did you call writing() with a pointer that isn't into a datafile?
        return 0;
    }

    bool MongoMMF::open(string fname) {
        view_write = map(fname.c_str());
        // temp : view_private pending more work!
        view_private = view_write;
        if( view_write ) { 
             if( durable ) {
               view_readonly = MemoryMappedFile::createReadOnlyMap();
                mutex::scoped_lock lk(our_views_mutex);
                our_read_views[view_readonly] = this; 
             }
            return true;
        }
        return false;
    }

    bool MongoMMF::create(string fname, unsigned long long& len) { 
        view_write = map(fname.c_str(), len);
        // temp : view_private pending more work!
        view_private = view_write;
        if( view_write ) {
            if( durable ) {
                view_readonly = MemoryMappedFile::createReadOnlyMap();
                mutex::scoped_lock lk(our_views_mutex);
                our_read_views[view_readonly] = this;
            }
            return true;
        }
        return false;
    }
    
    /* we will re-map the private few frequently, thus the use of MoveableBuffer */
    MoveableBuffer MongoMMF::getView() { 
        if( durable && debug )
            return view_readonly;
        return view_private;
    }

    MongoMMF::MongoMMF() {
        view_write = view_private = view_readonly = 0; 
    }

    MongoMMF::~MongoMMF() {
#if defined(_DEBUG) && defined(_DURABLE)
        {
            mutex::scoped_lock lk(our_views_mutex);
            our_read_views.erase(view_readonly);
        }
#endif
        view_write = view_private = view_readonly = 0; 
    }

}
