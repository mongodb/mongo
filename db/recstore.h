// recstore.h

#pragma once

#include "../util/file.h"

namespace mongo { 

using boost::uint32_t;
using boost::uint64_t;

/* Current version supports only consistent record sizes within a store. */

class BasicRecStore { 
    struct RecStoreHeader { 
        uint32_t version;
        uint32_t recsize;
        uint64_t leof; // logical eof, actual file might be prealloc'd further
        uint64_t firstDeleted; // 0 = no deleted recs
        uint32_t cleanShutdown; // 0 = clean
        char reserved[8192-8-8-4-4-4]; // we want our records page-aligned in the file if they are a multiple of a page's size -- so we make this 8KB with that goal
        RecStoreHeader() { 
            version = 65;
            recsize = 0;
            leof = sizeof(RecStoreHeader);
            firstDeleted = 0;
            cleanShutdown = 1;
            memset(reserved, 0, sizeof(reserved));
        }
    };

public:
    BasicRecStore(int _fileNumber) : fileNumber(_fileNumber) { }
    ~BasicRecStore();
    void init(const char *fn, unsigned recsize);
    fileofs insert(const char *buf, unsigned len);
    void update(fileofs o, const char *buf, unsigned len);
    void remove(fileofs o, unsigned len);
    void get(fileofs o, char *buf, unsigned len);

    int fileNumber; // this goes in DiskLoc::a

    string filename;

private:

    void writeHeader();
    File f;
    fileofs len;
    RecStoreHeader h; // h.reserved is wasteful here; fix later.
    void write(fileofs ofs, const char *data, unsigned len) { 
        f.write(ofs, data, len);
        massert("basicrecstore write io error", !f.bad());
    }
};

/* --- implementation --- */

inline BasicRecStore::~BasicRecStore() { 
    h.cleanShutdown = 0;
    if( f.is_open() ) {
        writeHeader();
        f.fsync();
    }
}

inline void BasicRecStore::writeHeader() { 
    write(0, (const char *) &h, 28); // update header in file for new leof
    uassert("file io error in BasicRecStore [1]", !f.bad()); 
}

inline fileofs BasicRecStore::insert(const char *buf, unsigned reclen) { 
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
        write(len, "", 0);
    }
    writeHeader();
    write(ofs, buf, reclen);
    uassert("file io error in BasicRecStore [2]", !f.bad());
    return ofs;
}

/* so far, it's ok to read or update a subset of a record */

inline void BasicRecStore::update(fileofs o, const char *buf, unsigned len) { 
    assert(o <= h.leof && o >= sizeof(RecStoreHeader));
    write(o, buf, len);
}

inline void BasicRecStore::get(fileofs o, char *buf, unsigned len) { 
    assert(o <= h.leof && o >= sizeof(RecStoreHeader));
    f.read(o, buf, len);
    massert("basicrestore::get I/O error", !f.bad());
}

inline void BasicRecStore::remove(fileofs o, unsigned len) { 
    uasserted("not yet implemented recstoreremove");
}

}
