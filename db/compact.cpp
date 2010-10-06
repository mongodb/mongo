/* @file compact.cpp
   compaction of deleted space in pdfiles (datafiles)
*/

/* NOTE 6Oct2010 : this file PRELIMINARY, EXPERIMENTAL, NOT DONE, NOT USED YET (not in SConstruct) */

/**
*    Copyright (C) 2010 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "pdfile.h"
#include "concurrency.h"

namespace mongo { 

    class CompactJob {
    public:
        CompactJob(string ns) : _ns(ns) { }
        void go();
    private:
        void clean(Extent *e);
        void phase1();
        const string _ns;
    };

    void CompactJob::clean(Extent *e) {
        //avoid = e->myLoc;

        while( 1 ) { 
            Record *r = e->getRecord( e->firstRecord );
            theDataFileMgr.deleteRecord(_ns.c_str(), r, e->firstRecord);
            ///....
        }

    }

    void CompactJob::phase1() { 
        writelock lk;
        //readlock lk;

        NamespaceDetails *nsd = nsdetails(_ns.c_str());
        if( nsd == 0 ) throw "ns gone";
        if( nsd->capped ) throw "capped compact not supported nor usually necessary";

        DiskLoc L = nsd->firstExtent;
        if( L.isNull() ) throw "nothing to do";

        Extent *e = L.ext();
        assert( e );
        Extent *ne = e->getNextExtent();
        if( ne == 0 ) throw "only 1 extent";

        clean(e);
    }

    void CompactJob::go() { 
        phase1();
    }

    void compactThread() {
        Client::initThread("compact");
        try { 
            CompactJob compact("test.foo");
            compact.go();
        }
        catch(const char *p) { 
            DEV log() << "info: exception compact " << p << endl;
        }
        catch(...) { 
            DEV log() << "info: exception compact" << endl;
        }
    }

}
