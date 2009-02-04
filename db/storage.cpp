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
    massert("not using", false);
    while( 1 ) { 
        sleepsecs(100);
        dblock lk;
        BasicCached_RecStore::rc.writeDirty();
        RecCache::tempStore.flush();
    }
}

// Currently only called on program exit.
void recCacheCloseAll() { 
    BasicCached_RecStore::rc.writeDirty( true );
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
        if( h.leof > len ) { 
            stringstream ss;
            ss << "bad recstore, file:" << fn << " leof:" << h.leof << " len:" << len;
            massert(ss.str(), false);
        }
        if( h.cleanShutdown )
            log() << "warning: non-clean shutdown for file " << fn << '\n';
        h.cleanShutdown = 2;
        writeHeader();
    }
    f.flush();
    //    boost::thread t(storeThread);
}

/* -------------------------------------------------------- */

inline void RecCache::writeIfDirty(Node *n) {
    if( n->dirty ) {
        n->dirty = false;
        tempStore.update(fileOfs(n->loc), n->data, recsize);
    }
}

/* note that this is written in order, as much as possible, given that dirtyl is of type set. */
void RecCache::writeDirty( bool rawLog ) { 
    try { 
        for( set<DiskLoc>::iterator i = dirtyl.begin(); i != dirtyl.end(); i++ ) { 
            map<DiskLoc, Node*>::iterator j = m.find(*i);
            if( j != m.end() )
                writeIfDirty(j->second);
        }
    }
    catch(...) {
        const char *message = "Problem: bad() in RecCache::writeDirty, file io error\n";
        if ( rawLog )
            rawOut( message );
        else
            ( log() << message ).flush();
    }
    dirtyl.clear();
}

// 100k * 8KB = 800MB
const unsigned RECCACHELIMIT = 150000;
//const unsigned RECCACHELIMIT = 25;

inline void RecCache::ejectOld() { 
    if( nnodes <= RECCACHELIMIT )
        return;
    Node *n = oldest;
    while( 1 ) {
        if( nnodes <= RECCACHELIMIT - 4 ) { 
            n->older = 0;
            oldest = n;
            assert( oldest ) ;
            break;
        }
        nnodes--;
        assert(n);
        Node *nxt = n->newer;
        writeIfDirty(n);
        m.erase(n->loc);
        delete n;
        n = nxt;
    }
}

void RecCache::dump() { 
    Node *n = oldest;
    Node *last = 0;
    while( n ) { 
        assert( n->older == last );
        last = n;
//        cout << n << ' ' << n->older << ' ' << n->newer << '\n';
        n=n->newer;
    }
    assert( newest == last );
//    cout << endl;
}

void dbunlocking() { 
    assert( dbMutexInfo.isLocked() );
    BasicCached_RecStore::rc.ejectOld();
}

}
