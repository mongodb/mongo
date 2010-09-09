/** @file capped.cpp
 */

#include "pch.h"
#include "capped.h"
#include "namespace.h"
#include "nonce.h"
#include "../util/mongoutils/str.h"
#include "../util/mmap.h"

using namespace mongoutils;

namespace mongo { 

    /* file is chunked up into pieces or "regions" this size. You cannot store a record larger than that. */
    const unsigned RegionSize = 256 * 1024 * 1024;

#pragma pack(1)
    /** each capped collection has its own file */
    struct FileHeader { 
        char ns[512];
        char reserved[512];
        unsigned ver1;
        unsigned ver2;
        unsigned long long fileSize;
        unsigned regionSize;
        unsigned nRegions;
        char reserved3[8192 - 1024 - 3*8];
    };
    struct RecordHeader { 
        unsigned long long ord;
        unsigned recordSize;
        // then: data[recordSize]
        // then: unsigned recordSizeRepeats;
    };
    struct Region {
        unsigned size; // size of this region. the last region in the file may be smaller than the rest
        unsigned zero;     // future
        char reserved[8192 - 8];
        union {
            char data[ RegionSize - 8192 ];
            RecordHeader firstRecord;
        };
    };
    struct File { 
        FileHeader fileHeader;
        Region regions[1];
    };
#pragma pack()

    class CappedCollection2 : boost::noncopyable {
    public:
        MMF file;
        MMF::Pointer ptr;

        ~CappedCollection2() { file.close(); }
        Region* regionHeader(unsigned i) { 
            size_t ofs = 8192 + i * RegionSize;
            return (Region *) ptr.at(ofs, 8);
        }
        RecordHeader* firstRecordHeaderForRegion(unsigned i) { 
            size_t ofs = 8192 + i * RegionSize + 8192;
            return (RecordHeader *) ptr.at(ofs, 8);
        }
    };

    inline CappedCollection2*& cc2(NamespaceDetails *nsd) { 
        return (CappedCollection2*&) nsd->capped2.cc2_ptr;
    }

    void findAFileNumber(boost::filesystem::path partialpath, unsigned& fileNumber, unsigned toTry) { 
        if( toTry == 0 ) return; // 0 not allowed that's our null sentinel
        string fname = (str::stream() << partialpath.string() << hex << toTry);
        if( MMF::exists(fname) ) 
            return;
        fileNumber = toTry;
    }

    string filename(string path, string db, unsigned fileNumber) { 
        boost::filesystem::path p(path);
        stringstream ss;
        ss << db << ".cap." << hex << fileNumber;
        p /= ss.str();
        return p.string();
    }

    void cappedcollection::open(string path, string db, NamespaceDetails *nsd) { 
        if( cc2(nsd) ) // already open
            return;
        assertInWriteLock();
        uassert(13466, str::stream() << "bad filenumber for capped collection in db " << db << " - try repair?", nsd->capped2.fileNumber);
        auto_ptr<CappedCollection2> cc( new CappedCollection2 );
        cc->ptr = cc->file.map( filename(path, db, nsd->capped2.fileNumber).c_str() );
        cc2(nsd) = cc.release();
    }

    void cappedcollection::close(NamespaceDetails *nsd) { 
        assertInWriteLock();
        CappedCollection2*& c = cc2(nsd);
        if( c ) {
            delete c;
            c = 0;
        }
    }

    void cappedcollection::create(string path, string ns, NamespaceDetails *nsd, unsigned long long sz) { 
        assertInWriteLock();
        boost::filesystem::path p(path);
        NamespaceString nsstr(ns);
        {
            stringstream ss;
            ss << nsstr.db << ".cap.";
            p /= ss.str();
        }

        massert(13467, str::stream() << "bad cappedcollection::create filenumber - try repair?" << ns, nsd->capped2.fileNumber == 0);

        // we start with small random #s just to be polite (not really important)
        unsigned n = 0;
        for( int i = 0; i < 3 && n == 0; i++ ) {
            findAFileNumber(p, n, ((unsigned) rand()) % 0x100);
        }
        for( int i = 0; i < 3 && n == 0; i++ ) {
            findAFileNumber(p, n, ((unsigned) rand()) % 0x1000);
        }
        for( int i = 0; i < 20 && n == 0; i++ ) {
            // our file #s for capped2 are 18 bits
            findAFileNumber(p, n, ((unsigned) security.getNonce()) % 0x40000);
        }
        if( n == 0 ) { 
            uasserted(10000, str::stream() << "couldn't find a file number to assign to new capped collection " << ns);
        }

        {
            unsigned long long realSize;
            if( sz >= 1024 * 1024 )
                realSize = sz & 0x1fff;
            else {
                realSize = (sz + 0x1fff) & (~0x1fffLL);
                assert(realSize >= sz);
                realSize += 16 * 1024;
            }

            auto_ptr<CappedCollection2> cc( new CappedCollection2 );
            cc->ptr = cc->file.create( filename(path, nsstr.db, n), realSize, true);
            {
                // init file header
                FileHeader *h = (FileHeader *) cc->ptr.at(0, 8192);
                if( ns.size() < sizeof(h->ns) )
                    strcpy(h->ns, ns.c_str());
                h->reserved[0] = 0x1a; // just make console output of the file nicer maybe...
                h->reserved[1] = 4;
                h->ver1 = 1;
                h->ver2 = 1;
                h->fileSize = realSize;
                h->regionSize = RegionSize;
                {
                    unsigned long long net = (realSize - 8192);
                    unsigned long long nr = net / RegionSize;
                    if( nr == 0 )
                        nr = 1;
                    h->nRegions = (unsigned) nr;
                }

                // init region headers
                for( unsigned r = 0; r < h->nRegions-1; r++ ) {
                    Region *region = cc->regionHeader(r);
                    region->size = RegionSize;
                }
                unsigned long long left = realSize - 8192 - (h->nRegions - 1) * RegionSize;
                assert( left > 0 && left <= RegionSize );
                cc->regionHeader(h->nRegions-1)->size = (unsigned) left;
            }
            cc2(nsd) = cc.release();
            nsd->capped2.fileNumber = n;
        }
    }

}
