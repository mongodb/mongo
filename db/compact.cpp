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
#include "commands.h"
#include "curop-inl.h"
#include "../util/concurrency/task.h"

namespace mongo {

    class CompactJob {
    public:
        CompactJob(string ns, BSONObjBuilder& result) : _ns(ns), _result(result) { }
        void go();
    private:
        BSONObjBuilder& _result; // return value sent to the client from the compact command
        const string _ns;
        NamespaceDetails * nsd();
        void doBatch();
        void prep();
        //unsigned long long _nrecords;
        //unsigned long long _ncompacted;
        //DiskLoc _firstExtent;
    };

    // lock & set context first.  this checks that collection still exists, and that it hasn't
    // morphed into a capped collection between locks (which is possible)
    NamespaceDetails * CompactJob::nsd() {
        NamespaceDetails *nsd = nsdetails(_ns.c_str());
        if( nsd == 0 ) 
            throw "ns no longer present";
        if( nsd->firstExtent.isNull() )
            throw "no first extent";
        if( nsd->capped )
            throw "capped collection";
        return nsd;
    }

    void CompactJob::doBatch() {

        {
            readlock lk;
            Client::Context ctx(_ns);
            NamespaceDetails *d = nsd();
            Extent *e = d->firstExtent.ext();
            DiskLoc L = e->firstRecord;
            while( 1 ) { 
                if( L.isNull() ) 
                    break;
                log() << L.toString() << endl;
                L = L.rec()->nextInExtent(L);
            }
        }


#if 0
        unsigned n = 0;
        if( 0 ) {
            /* pre-touch records in a read lock so that paging happens in read not write lock.
               note we are only touching the records though; if indexes aren't in RAM, they will
               page later.  So the concept is only partial.
               */
            readlock lk;
            Timer t;
            Client::Context ctx(_ns);
            NamespaceDetails *nsd = beginBlock();
            if( nsd->firstExtent != _firstExtent )  {
                // TEMP DEV - stop after 1st extent
                throw "change of first extent";
            }
            DiskLoc loc = nsd->firstExtent.ext()->firstRecord;
            while( !loc.isNull() ) {
                Record *r = loc.rec();
                loc = r->getNext(loc);
                if( ++n >= 100 || (n % 8 == 0 && t.millis() > 50) )
                    break;
            }
        }
        if( 0 ) {
            writelock lk;
            Client::Context ctx(_ns);
            NamespaceDetails *nsd = beginBlock();
            for( unsigned i = 0; i < n; i++ ) {
                if( nsd->firstExtent != _firstExtent )  {
                    // TEMP DEV - stop after 1st extent
                    throw "change of first extent (or it is now null)";
                }
                DiskLoc loc = nsd->firstExtent.ext()->firstRecord;
                Record *rec = loc.rec();
                BSONObj o = loc.obj().getOwned(); // todo: inefficient, double mem copy...
                try {
                    theDataFileMgr.deleteRecord(_ns.c_str(), rec, loc, false);
                }
                catch(DBException&) { throw "error deleting record"; }
                try {
                    theDataFileMgr.insertNoReturnVal(_ns.c_str(), o);
                }
                catch(DBException&) {
                    /* todo: save the record somehow??? try again with 'avoid' logic? */
                    log() << "compact: error re-inserting record ns:" << _ns << " n:" << _nrecords << " _id:" << o["_id"].toString() << endl;
                    throw "error re-inserting record";
                }
                ++_ncompacted;
                if( killCurrentOp.globalInterruptCheck() )
                    throw "interrupted";
            }
        }
#endif
    }

    void CompactJob::prep() {
        readlock lk(_ns);
        Client::Context ctxt(_ns);
        NamespaceDetails *d = nsd();

        DiskLoc L = d->firstExtent;
        int ne = 0;
        if( !L.isNull() ) {
            Extent *e = L.ext();
            do {
                ne++;
                e = e->getNextExtent();
            } while( e );
        }

        _result.append("nExtents", ne);
        //        
        //        assert( !L.isNull() );
        //        _firstExtent = L;
        //_nrecords = nsd->stats.nrecords;
        //_ncompacted = 0;
    }

    void CompactJob::go() {
        //cc().curop()->reset();
        //cc().curop()->setNS(_ns.c_str());
        //cc().curop()->markCommand();
        try {
            prep();
            //while( 1 )
                doBatch();
        }
        catch(const char *p) {
            log() << "info: exception compact " << p << endl;
        }
        catch(...) {
            log() << "info: exception compact" << endl;
        }
    }

    static mutex m("compact");

    class CompactCmd : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual void help( stringstream& help ) const {
            help << "compact / defragment a collection in the background, slowly, attempting to minimize disruptions to other operations\n"
                 "{ compact : <collection> }";
        }
        virtual bool requiresAuth() { return true; }
        CompactCmd() : Command("compact") { }

        virtual bool run(const string& db, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string coll = cmdObj.firstElement().valuestr();
            if( coll.empty() || db.empty() ) {
                errmsg = "no collection name specified";
                return false;
            }
            string ns = db + '.' + coll;
            assert( isANormalNSName(ns.c_str()) );
            assert( !str::contains(ns, ".system.") ); // items in system.indexes cannot be moved there are pointers to those disklocs in NamespaceDetails
            {
                readlock lk;
                Client::Context ctx(ns);
                if( nsdetails(ns.c_str()) == 0 ) {
                    errmsg = "namespace " + ns + " does not exist";
                    return false;
                }
            }

            mutex::try_lock lk(m);
            if( !lk.ok ) {
                errmsg = "a compaction is already running";
                return false;
            }
            CompactJob j(ns, result);
            j.go();
            errmsg = "compact is not yet implemented";
            return false;
        }
    };
    static CompactCmd compactCmd;

}
