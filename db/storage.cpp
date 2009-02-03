// storage.cpp

#include "stdafx.h"
#include "pdfile.h"
#include "reccache.h"
#include "rec.h"
#include "db.h"

namespace mongo {

BasicRecStore RecCache::tempStore;
RecCache BasicCached_RecStore::rc(BucketSize);

static void storeThread() { 
    while( 1 ) { 
        sleepsecs(1);
        dblock lk;
        BasicCached_RecStore::rc.writeDirty();
        RecCache::tempStore.flush();
    }
}

void recCacheCloseAll() { 
    BasicCached_RecStore::rc.writeDirty();
    RecCache::tempStore.flush();
}

void BasicRecStore::init(const char *fn, unsigned recsize)
{ 
    massert( "compile packing problem recstore?", sizeof(RecStoreHeader) == 8192);
    unsigned flags = ios::binary | ios::in | ios::out;
    if( boost::filesystem::exists(fn) )
        flags |= ios::ate;
    else
        flags |= ios::trunc;
    f.open(fn, (ios_base::openmode) flags);
    uassert( string("couldn't open file:")+fn, f.is_open() );
    len = f.tellg();
    if( len == 0 ) { 
        log() << "creating recstore file " << fn << '\n';
        h.recsize = recsize;
        len = sizeof(RecStoreHeader);
        f.seekp(0);
        f.write((const char *) &h, sizeof(RecStoreHeader));
    }    
    else { 
        f.seekg(0);
        f.read((char *) &h, sizeof(RecStoreHeader));
        massert(string("recstore recsize mismatch, file:")+fn, h.recsize == recsize);
        massert(string("bad recstore [1], file:")+fn, (h.leof-sizeof(RecStoreHeader)) % recsize == 0);        
        massert(string("bad recstore [2], file:")+fn, h.leof <= len);
        if( h.cleanShutdown )
            log() << "warning: non-clean shutdown for file " << fn << '\n';
        h.cleanShutdown = 2;
        writeHeader();
    }
    f.flush();
    boost::thread t(storeThread);
}

inline void RecCache::writeIfDirty(Node *n) {
    if( n->dirty ) {
        n->dirty = false;
        tempStore.update(fileOfs(n->loc), n->data, recsize);
    }
}

void RecCache::writeDirty() { 
    try { 
        for( list<DiskLoc>::iterator i = dirtyl.begin(); i != dirtyl.end(); i++ ) { 
            map<DiskLoc, Node*>::iterator j = m.find(*i);
            if( j != m.end() )
                writeIfDirty(j->second);
        }
    }
    catch(...) {
        log() << "Problem: bad() in RecCache::writeDirty, file io error\n";
    }
    dirtyl.clear();
}

// 10k * 8KB = 80MB
const unsigned RECCACHELIMIT = 10000;

inline void RecCache::ejectOld() { 
    if( nnodes <= RECCACHELIMIT )
        return;
    Node *n = oldest;
    while( 1 ) {
        if( nnodes <= RECCACHELIMIT ) { 
            n->older = 0;
            oldest = n;
            break;
        }
        nnodes--;
        Node *nxt = n->newer;
        assert(n);
        writeIfDirty(n);
        m.erase(n->loc);
        delete n;
        n = nxt;
    }
}

void dbunlocked() { 
    BasicCached_RecStore::rc.ejectOld();
}

}
