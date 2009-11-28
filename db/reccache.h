// reccache.h

/* CachedBasicRecStore
   This is our store which implements a traditional page-cache type of storage
   (not memory mapped files).
*/

/* LOCK HIERARCHY
     
     dblock
       RecCache::rcmutex

     i.e. always lock dblock first if you lock both

*/

#pragma once

#include "reci.h"
#include "recstore.h"

namespace mongo { 

class RecCache {
    struct Node { 
        Node(void* _data) : data((char *) _data) { dirty = false; newer = 0; }
        ~Node() { 
            free(data);
            data = 0;
        }
        char *data;
        DiskLoc loc;
        bool dirty;
        Node *older, *newer; // lru
    };
    boost::mutex &rcmutex; // mainly to coordinate with the lazy writer thread
    unsigned recsize;
    map<DiskLoc, Node*> m; // the cache
    Node *newest, *oldest;
    unsigned nnodes;
    set<DiskLoc> dirtyl;
    vector<BasicRecStore*> stores; // DiskLoc::a() indicates the index into this vector
    map<string, BasicRecStore*> storesByNsKey; // nskey -> BasicRecStore*
public:
    static unsigned MAXNODES;
    enum BaseValue { Base = 10000 };
private:
    BasicRecStore* _initStore(string fname);
    BasicRecStore* initStore(int n);
    string findStoreFilename(const char *_ns, bool& found);
    void initStoreByNs(const char *ns, const string& nskey);
    void closeStore(BasicRecStore *rs);

    static string directory();
    static string mknskey(const char *ns) { 
        return directory() + ns;
    }

    /* get the right file for a given diskloc */
    BasicRecStore& store(DiskLoc& d) { 
        int n = d.a() - Base;
        if( (int) stores.size() > n ) { 
            BasicRecStore *rs = stores[n];
            if( rs ) {
                assert( rs->fileNumber == n );
                return *rs;
            }
        }
        return *initStore(n);
    }
    BasicRecStore& store(const char *ns) {
        string nskey = mknskey(ns);
        BasicRecStore *&rs = storesByNsKey[nskey];
        if( rs )
            return *rs;
        initStoreByNs(ns, nskey);
        return *rs;
    }

    void writeDirty( set<DiskLoc>::iterator i, bool rawLog = false );
    void writeIfDirty(Node *n);
    void touch(Node* n) { 
        if( n == newest )
            return;
        if( n == oldest ) {
            oldest = oldest->newer;
            assert( oldest || nnodes == 1 );
        }
        if( n->older ) 
            n->older->newer = n->newer;
        if( n->newer ) 
            n->newer->older = n->older;
        n->newer = 0;        
        n->older = newest;
        newest->newer = n;
        newest = n;
    }
    Node* mkNode() { 
        Node *n = new Node(calloc(recsize,1)); // calloc is TEMP for testing.  change to malloc
        n->older = newest;
        if( newest )
            newest->newer = n;
        else {
            assert( oldest == 0 );
            oldest = n;
        }
        newest = n;
        nnodes++;
        return n;
    }
    fileofs fileOfs(DiskLoc d) { 
        return ((fileofs) d.getOfs()) * recsize;
    }

    void dump();
    void _ejectOld();

public:
    /* all public functions (except constructor) should use the mutex */

    RecCache(unsigned recsz) : rcmutex( *( new boost::mutex() ) ), recsize(recsz) { 
        nnodes = 0;
        newest = oldest = 0;
    }

    /* call this after doing some work, after you are sure you are done with modifications.
       we call it from dbunlocking().
    */
    void ejectOld() { 
        if( nnodes > MAXNODES ) // just enough here to be inlineable for speed reasons.  _ejectOld does the real work
            _ejectOld();
    }

    /* bg writer thread invokes this */
    void writeLazily();

    /* Note that this may be called BEFORE the actual writing to the node 
       takes place.  We do flushing later on a dbunlocking() call, which happens 
       after the writing.
    */
    void dirty(DiskLoc d) {
        assert( d.a() >= Base );
        boostlock lk(rcmutex);
        map<DiskLoc, Node*>::iterator i = m.find(d);
        if( i != m.end() ) {
            Node *n = i->second;
            if( !n->dirty ) { 
                n->dirty = true;
                dirtyl.insert(n->loc);
            }
        }
    }

    char* get(DiskLoc d, unsigned len) { 
        assert( d.a() >= Base );
        assert( len == recsize );

        boostlock lk(rcmutex);
        map<DiskLoc, Node*>::iterator i = m.find(d);
        if( i != m.end() ) {
            touch(i->second);
            return i->second->data;
        }

        Node *n = mkNode();
        n->loc = d;
        store(d).get(fileOfs(d), n->data, recsize); // could throw exception
        m.insert( pair<DiskLoc, Node*>(d, n) );
        return n->data;
    }

    void drop(const char *ns);

    DiskLoc insert(const char *ns, const void *obuf, int len, bool god) {
        boostlock lk(rcmutex);
        BasicRecStore& rs = store(ns);
        fileofs o = rs.insert((const char *) obuf, len);
        assert( o % recsize == 0 );
        fileofs recnum = o / recsize;
        massert( "RecCache file too large?", recnum <= 0x7fffffff );
        Node *n = mkNode();
        memcpy(n->data, obuf, len);
        DiskLoc d(rs.fileNumber + Base, (int) recnum);
        n->loc = d;
        m[d] = n;
        return d;
    }

    void closeFiles(string dbname, string path);

    // at termination: write dirty pages and close all files
    void closing();
};

extern RecCache theRecCache;

class CachedBasicRecStore : public RecStoreInterface { 
public:
    virtual char* get(DiskLoc d, unsigned len) { 
        return theRecCache.get(d, len);
    }

    virtual DiskLoc insert(const char *ns, const void *obuf, int len, bool god) { 
        return theRecCache.insert(ns, obuf, len, god);
    }

    virtual void modified(DiskLoc d) { 
        theRecCache.dirty(d);
    }

    /* drop collection */
    virtual void drop(const char *ns) { 
        theRecCache.drop(ns);
    }

    virtual void rename(const char *fromNs, const char *toNs) {
      massert( "rename not yet implemented for CachedBasicRecStore", false );
    }

    /* close datafiles associated with the db specified. */
    virtual void closeFiles(string dbname, string path) {
        theRecCache.closeFiles(dbname, dbpath);
    }
};

/* see concurrency.h - note on a lock reset from read->write we don't 
   call dbunlocking_read, we just wait for the final dbunlocking_write 
   call 
*/

inline void dbunlocking_read() { 
    Client *c = currentClient.get();
    c->top.clientStop();
}

inline void dbunlocking_write() { 
    theRecCache.ejectOld();
	dbunlocking_read();
}

} /*namespace*/
