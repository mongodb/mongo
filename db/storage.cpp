// storage.cpp

#include "stdafx.h"
#include "pdfile.h"
#include "reccache.h"
#include "rec.h"
#include "db.h"

namespace mongo {

void writerThread();

#if defined(_RECSTORE)
    static int inited;
#endif

// pick your store for indexes by setting this typedef
// this doesn't need to be an ifdef, we can make it dynamic
#if defined(_RECSTORE)
RecStoreInterface *btreeStore = new CachedBasicRecStore();
#else
RecStoreInterface *btreeStore = new MongoMemMapped_RecStore();
#endif

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

}
