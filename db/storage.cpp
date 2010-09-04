// storage.cpp
/*
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


#include "pch.h"
#include "pdfile.h"
#include "rec.h"
#include "db.h"

namespace mongo {

// pick your store for indexes by setting this typedef
// this doesn't need to be an ifdef, we can make it dynamic
#if defined(_RECSTORE)
RecStoreInterface *btreeStore = new CachedBasicRecStore();
#else
MongoMemMapped_RecStore *btreeStore = new MongoMemMapped_RecStore();
#endif

#if 0

#if defined(_RECSTORE)
    static int inited;
#endif

void writerThread();

void BasicRecStore::init(const char *fn, unsigned recsize)
{ 
    massert( 10394 ,  "compile packing problem recstore?", sizeof(RecStoreHeader) == 8192);
    filename = fn;
    f.open(fn);
    uassert( 10130 ,  string("couldn't open file:")+fn, f.is_open() );
    len = f.len();
    if( len == 0 ) { 
        log() << "creating recstore file " << fn << '\n';
        h.recsize = recsize;
        len = sizeof(RecStoreHeader);
        f.write(0, (const char *) &h, sizeof(RecStoreHeader));
    }    
    else { 
        f.read(0, (char *) &h, sizeof(RecStoreHeader));
        massert( 10395 , string("recstore was not closed cleanly: ")+fn, h.cleanShutdown==0);
        massert( 10396 , string("recstore recsize mismatch, file:")+fn, h.recsize == recsize);
        massert( 10397 , string("bad recstore [1], file:")+fn, (h.leof-sizeof(RecStoreHeader)) % recsize == 0);        
        if( h.leof > len ) { 
            stringstream ss;
            ss << "bad recstore, file:" << fn << " leof:" << h.leof << " len:" << len;
            massert( 10398 , ss.str(), false);
        }
        if( h.cleanShutdown )
            log() << "warning: non-clean shutdown for file " << fn << '\n';
        h.cleanShutdown = 2;
        writeHeader();
        f.fsync();
    }
#if defined(_RECSTORE)
    if( inited++ == 0 ) {
        boost::thread t(writerThread);
    }
#endif
}

#endif

}
