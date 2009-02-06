// storage.cpp

#include "stdafx.h"
#include "pdfile.h"
#include "reccache.h"
#include "rec.h"
#include "db.h"

namespace mongo {

RecCache theRecCache(BucketSize);

void writerThread() { 
    sleepsecs(10);
    while( 1 ) { 
        try { 
            theRecCache.writeLazily();
        }
        catch(...) { 
            log() << "exception in writerThread()" << endl;
            sleepsecs(3);
        }
    }
}

// called on program exit.
void recCacheCloseAll() { 
#if defined(_RECSTORE)
    theRecCache.closing();
#endif
}

int ndirtywritten;

inline void RecCache::writeIfDirty(Node *n) {
    if( n->dirty ) {
        ndirtywritten++;
        n->dirty = false;
        tempStore.update(fileOfs(n->loc), n->data, recsize);
    }
}

/* note that this is written in order, as much as possible, given that dirtyl is of type set. */
void RecCache::writeDirty( set<DiskLoc>::iterator startAt, bool rawLog ) { 
    try { 
        ndirtywritten=0;
        for( set<DiskLoc>::iterator i = startAt; i != dirtyl.end(); i++ ) { 
            map<DiskLoc, Node*>::iterator j = m.find(*i);
            if( j != m.end() )
                writeIfDirty(j->second);
        }
        OCCASIONALLY out() << "TEMP: ndirtywritten: " << ndirtywritten << endl;
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

void RecCache::writeLazily() {
    int sleep = 0;
    int k;
    {
        boostlock lk(rcmutex);
        Timer t;
        set<DiskLoc>::iterator i = dirtyl.end();
        for( k = 0; k < 100; k++ ) {
            if( i == dirtyl.begin() ) { 
                // we're not very far behind
                sleep = k < 20 ? 2000 : 1000;
                break;
            }
            i--;
        }
        writeDirty(i);
        if( sleep == 0 ) {
            sleep = t.millis() * 4 + 10;
        }
    }

    OCCASIONALLY cout << "writeLazily " << k << " sleep:" << sleep << '\n';
    sleepmillis(sleep);
}

// 100k * 8KB = 800MB
const unsigned RECCACHELIMIT = 150000;

inline void RecCache::ejectOld() { 
    if( nnodes <= RECCACHELIMIT )
        return;
    boostlock lk(rcmutex);
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
    dassert( dbMutexInfo.isLocked() );
    theRecCache.ejectOld();
}

}
