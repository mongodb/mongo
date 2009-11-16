// storage.cpp

#include "stdafx.h"
#include "pdfile.h"
#include "reccache.h"
#include "rec.h"
#include "db.h"

namespace mongo {

RecCache theRecCache(BucketSize);

// 100k * 8KB = 800MB
unsigned RecCache::MAXNODES = 50000;

void setRecCacheSize(unsigned mb) {
    unsigned long long MB = mb;
    log(2) << "reccache size: " << MB << "MB\n";
    uassert( "bad cache size", MB > 0 && MB < 1000000 );
    RecCache::MAXNODES = (unsigned) MB * 1024 * 1024 / 8192;
    log(3) << "RecCache::MAXNODES=" << RecCache::MAXNODES << '\n';
}

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

inline static string escape(const char *ns) {
    char buf[256];
    char *p = buf;
    while( 1 ) {
        if( *ns == '$' ) *p = '~';
        else
            *p = *ns;
        if( *ns == 0 )
            break;
        p++; ns++;
    }
    assert( p - buf < (int) sizeof(buf) );
    return buf;
}

inline static string unescape(const char *ns) {
    char buf[256];
    char *p = buf;
    while( 1 ) {
        if( *ns == '~' ) *p = '$';
        else
            *p = *ns;
        if( *ns == 0 )
            break;
        p++; ns++;
    }
    assert( p - buf < (int) sizeof(buf) );
    return buf;
}

string RecCache::directory() { 
    return cc().database()->path;
}

/* filename format is 

     <n>-<ns>.idx
*/

BasicRecStore* RecCache::_initStore(string fname) { 

    assert( strchr(fname.c_str(), '/') == 0 );
    assert( strchr(fname.c_str(), '\\') == 0 );

    stringstream ss(fname);
    int n;
    ss >> n;
    assert( n >= 0 );
    char ch;
    ss >> ch;
    assert( ch == '-' );
    string rest;
    ss >> rest;
    const char *p = rest.c_str();
    const char *q = strstr(p, ".idx");
    assert( q );
    string escaped_ns(p, q-p);

    // arbitrary limit.  if you are hitting, we should use fewer files and put multiple 
    // indexes in a single file (which is easy to do)
    massert( "too many index files", n < 10000 );

    if( stores.size() < (unsigned)n+1 )
        stores.resize(n+1);
    assert( stores[n] == 0 );
    BasicRecStore *rs = new BasicRecStore(n);
    path pf(directory());
    pf /= fname;
    string full = pf.string();
    rs->init(full.c_str(), recsize);
    stores[n] = rs;
    string ns = unescape(escaped_ns.c_str());
    storesByNsKey[mknskey(ns.c_str())] = rs;
    return rs;
}

BasicRecStore* RecCache::initStore(int n) { 
    string ns;
    { 
        stringstream ss;
        ss << '/' << n << '-';
        ns = ss.str();
    }

    /* this will be slow if there are thousands of files */
    path dir(directory());
    directory_iterator end;
    try {
        directory_iterator i(dir);
        while ( i != end ) {
            string s = i->string();
            const char *p = strstr(s.c_str(), ns.c_str());
            if( p && strstr(p, ".idx") ) { 
                // found it
                path P = *i;
                return _initStore(P.leaf());
            }
            i++;
        }
    }
    catch( DBException & ) { 
        throw;
    }
    catch (...) {
        string s = string("i/o error looking for .idx file in ") + directory();
        massert(s, false);
    }
    stringstream ss;
    ss << "index datafile missing? n=" << n;
    uasserted(ss.str());
    return 0;
}

/* find the filename for a given ns.
   format is 
     <n>-<escaped_ns>.idx
   returns filename.  found is true if found.  If false, a proposed name is returned for (optional) creation
   of the file.
*/
string RecCache::findStoreFilename(const char *_ns, bool& found) {
    string namefrag;
    { 
        stringstream ss;
        ss << '-';
        ss << escape(_ns);
        ss << ".idx";
        namefrag = ss.str();
    }

    path dir(directory());
    directory_iterator end;
    int nmax = -1;
    try {
        directory_iterator i(dir);
        while ( i != end ) {
            string s = path(*i).leaf();
            const char *p = strstr(s.c_str(), namefrag.c_str());
            if( p ) {
                found = true;
                return s;
            }
            if( strstr(s.c_str(), ".idx") ) { 
                stringstream ss(s);
                int n = -1;
                ss >> n;
                if( n > nmax )
                    nmax = n;
            }
            i++;
        }
    }
    catch (...) {
        string s = string("i/o error looking for .idx file in ") + directory();
        massert(s, false);
    }

    // DNE.  return a name that would work.
    stringstream ss;
    ss << nmax+1 << namefrag;
    found = false;
    return ss.str();
}

void RecCache::initStoreByNs(const char *_ns, const string& nskey) {
    bool found;
    string fn = findStoreFilename(_ns, found);
    _initStore(fn);
}

inline void RecCache::writeIfDirty(Node *n) {
    if( n->dirty ) {
        ndirtywritten++;
        n->dirty = false;
        store(n->loc).update(fileOfs(n->loc), n->data, recsize);
    }
}

void RecCache::closeFiles(string dbname, string path) { 
    assertInWriteLock();
    boostlock lk(rcmutex);

    // first we write all dirty pages.  it is not easy to check which Nodes are for a particular
    // db, so we just write them all.
    writeDirty( dirtyl.begin(), true );

    string key = path + dbname + '.';
    unsigned sz = key.size();
    for( map<string, BasicRecStore*>::iterator i = storesByNsKey.begin(); i != storesByNsKey.end(); i++ ) { 
        map<string, BasicRecStore*>::iterator j = i;
        i++;
        if( strncmp(j->first.c_str(), key.c_str(), sz) == 0 ) {
            assert( stores[j->second->fileNumber] != 0 );
            stores[j->second->fileNumber] = 0;
            delete j->second;
            storesByNsKey.erase(j);
        }
    }
}

void RecCache::closing() { 
    boostlock lk(rcmutex);
    (cout << "TEMP: recCacheCloseAll() writing dirty pages...\n").flush();
    writeDirty( dirtyl.begin(), true );
    for( unsigned i = 0; i < stores.size(); i++ ) { 
        if( stores[i] ) {
            delete stores[i];
        }
    }
    (cout << "TEMP: write dirty done\n").flush();
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

void RecCache::_ejectOld() { 
    boostlock lk(rcmutex);
    if( nnodes <= MAXNODES )
        return;
    Node *n = oldest;
    while( 1 ) {
        if( nnodes <= MAXNODES - 4 ) { 
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

/* cleans up everything EXCEPT storesByNsKey.
   note this function is slow should not be invoked often
*/
void RecCache::closeStore(BasicRecStore *rs) { 
    int n = rs->fileNumber + Base;
    for( set<DiskLoc>::iterator i = dirtyl.begin(); i != dirtyl.end(); ) { 
        DiskLoc k = *i++;
        if( k.a() == n )
            dirtyl.erase(k);
    }

    for( map<DiskLoc,Node*>::iterator i = m.begin(); i != m.end(); ) { 
        DiskLoc k = i->first;
        i++;
        if( k.a() == n )
            m.erase(k);
    }

    assert( stores[rs->fileNumber] != 0 );
    stores[rs->fileNumber] = 0;
/*
    for( unsigned i = 0; i < stores.size(); i++ ) { 
        if( stores[i] == rs ) { 
            stores[i] = 0;
            break;
        }
    }*/
    delete rs; // closes file
}

void RecCache::drop(const char *_ns) { 
    // todo: test with a non clean shutdown file
    boostlock lk(rcmutex);

    map<string, BasicRecStore*>::iterator it = storesByNsKey.find(mknskey(_ns));
    string fname;
    if( it != storesByNsKey.end() ) {
        fname = it->second->filename;
        closeStore(it->second); // cleans up stores[] etc.
        storesByNsKey.erase(it);
    }
    else { 
        bool found;
        fname = findStoreFilename(_ns, found);
        if( !found ) { 
            log() << "RecCache::drop: no idx file found for " << _ns << endl;
            return;
        }
        path pf(directory());
        pf /= fname;
        fname = pf.string();
    }
    try {
        if( !boost::filesystem::exists(fname) ) 
            log() << "RecCache::drop: can't find file to remove " << fname << endl;
        boost::filesystem::remove(fname);
    } 
    catch(...) { 
        log() << "RecCache::drop: exception removing file " << fname << endl;
    }
}

}
