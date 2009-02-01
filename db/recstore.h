// recstore.h

#pragma once

namespace mongo { 

typedef uint64_t fileofs;

struct RecStoreHeader { 
    uint32_t version;
    uint32_t recsize;
    uint64_t leof; // logical eof, actual file might be prealloc'd further
    uint64_t firstDeleted; // 0 = no deleted recs
    uint32_t cleanShutdown; //  = clean
    char reserved[8192-8-8-4-4]; // we want our records page-aligned in the file if they are a multiple of a page's size -- so we make this 8KB with that goal
    RecStoreHeader() { 
        version = 65;
        recsize = 0;
        leof = sizeof(RecStoreHeader);
        firstDeleted = 0;
        cleanShutdown = 1;
        memset(reserved, 0, sizeof(reserved));
    }
};

/* Current version supports only consistent record sizes within a store. */

class RecStore { 
public:
    ~RecStore();
    RecStore(const char *fn, unsigned recsize);
    fileofs insert(const char *buf, unsigned len);
    void update(fileofs o, const char *buf, unsigned len);
    void remove(fileofs o, unsigned len);
private:
    void writeHeader();
    fstream f;
    fileofs len;
    RecStoreHeader h; // h.reserved is wasteful here; fix later.
};

/* --- implementation --- */

inline RecStore::~RecStore() { 
    h.cleanShutdown = 0;
    writeHeader();
}

inline 
RecStore::RecStore(const char *fn, unsigned recsize) : 
    f(fn, ios::ate | ios::binary | ios::in | ios::out) 
{ 
    massert( "compile packing problem recstore?", sizeof(RecStoreHeader) == 512);
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
        massert(string("bad recstore [1], file:")+fn, h.leof <= len);
        if( h.cleanShutdown )
            log() << "warning: non-clean shutdown for file " << fn << '\n';
        h.cleanShutdown = 2;
        writeHeader();
    }
}

inline void RecStore::writeHeader() { 
    f.seekp(0);
    f.write((const char *) &h, 28); // update header in file for new leof
    uassert("file io error in RecStore [1]", !f.bad()); 
}

inline fileofs RecStore::insert(const char *buf, unsigned reclen) { 
    if( h.firstDeleted ) { 
        uasserted("deleted not yet implemented recstoreinsert");
    }
    massert("bad len", reclen == h.recsize);
    fileofs ofs = h.leof;
    h.leof += reclen;
    if( h.leof > len ) { 
        // grow the file.  we grow quite a bit to avoid excessive file system fragmentations
        len += (len / 8) + h.recsize;
        uassert( "recstore file too big for 32 bit", len <= 0x7fffffff || sizeof(std::streamoff) > 4 );
        f.seekp((std::streamoff)len);
        f.write("", 0);
    }
    writeHeader();
    f.seekp((std::streamoff)ofs);
    f.write(buf, reclen);
    uassert("file io error in RecStore [2]", !f.bad());
    return ofs;
}

inline void RecStore::update(fileofs o, const char *buf, unsigned len) { 
    assert(o <= h.leof && o >= sizeof(RecStoreHeader));
    f.seekp((std::streamoff)o);
    f.write(buf, len);
}

inline void RecStore::remove(fileofs o, unsigned len) { 
    uasserted("not yet implemented recstoreremove");
}

}
