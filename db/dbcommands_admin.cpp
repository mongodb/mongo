// dbcommands_admin.cpp

/**
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
   this file has dbcommands that are for dba type administration
   mostly around dbs and collections
   NOT system stuff
*/


#include "stdafx.h"
#include "jsobj.h"
#include "pdfile.h"
#include "namespace.h"
#include "commands.h"
#include "cmdline.h"
#include "btree.h"

namespace mongo {

    class CleanCmd : public Command {
    public:
        CleanCmd() : Command( "clean" ){}

        virtual bool slaveOk(){ return true; }

        bool run(const char *nsRaw, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string dropns = cc().database()->name + "." + cmdObj.firstElement().valuestrsafe();
            
            if ( !cmdLine.quiet )
                log() << "CMD: clean " << dropns << endl;
            
            NamespaceDetails *d = nsdetails(dropns.c_str());
            
            if ( ! d ){
                errmsg = "ns not found";
                return 0;
            }

            for ( int i = 0; i < Buckets; i++ )
                d->deletedList[i].Null();

            result.append("ns", dropns.c_str());
            return 1;
        }
        
    } cleanCmd;
    
    class ValidateCmd : public Command {
    public:
        ValidateCmd() : Command( "validate" ){}

        virtual bool slaveOk(){
            return true;
        }
        
        //{ validate: "collectionnamewithoutthedbpart" [, scandata: <bool>] } */
        
        bool run(const char *nsRaw, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string ns = cc().database()->name + "." + cmdObj.firstElement().valuestrsafe();
            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( !cmdLine.quiet )
                log() << "CMD: validate " << ns << endl;

            if ( ! d ){
                errmsg = "ns not found";
                return 0;
            }
            
            result.append( "ns", ns );
            result.append( "result" , validateNS( ns.c_str() , d, &cmdObj ) );
            return 1;
        }
                    
        
        string validateNS(const char *ns, NamespaceDetails *d, BSONObj *cmdObj) {
            bool scanData = true;
            if( cmdObj && cmdObj->hasElement("scandata") && !cmdObj->getBoolField("scandata") )
                scanData = false;
            bool valid = true;
            stringstream ss;
            ss << "\nvalidate\n";
            ss << "  details: " << hex << d << " ofs:" << nsindex(ns)->detailsOffset(d) << dec << endl;
            if ( d->capped )
                ss << "  capped:" << d->capped << " max:" << d->max << '\n';
            
            ss << "  firstExtent:" << d->firstExtent.toString() << " ns:" << d->firstExtent.ext()->nsDiagnostic.buf << '\n';
            ss << "  lastExtent:" << d->lastExtent.toString()    << " ns:" << d->lastExtent.ext()->nsDiagnostic.buf << '\n';
            try {
                d->firstExtent.ext()->assertOk();
                d->lastExtent.ext()->assertOk();
                
                DiskLoc el = d->firstExtent;
                int ne = 0;
                while( !el.isNull() ) {
                    Extent *e = el.ext();
                    e->assertOk();
                    el = e->xnext;
                    ne++;
                    checkForInterrupt();
                }
                ss << "  # extents:" << ne << '\n';
            } catch (...) {
                valid=false;
                ss << " extent asserted ";
            }

            ss << "  datasize?:" << d->datasize << " nrecords?:" << d->nrecords << " lastExtentSize:" << d->lastExtentSize << '\n';
            ss << "  padding:" << d->paddingFactor << '\n';
            try {

                try {
                    ss << "  first extent:\n";
                    d->firstExtent.ext()->dump(ss);
                    valid = valid && d->firstExtent.ext()->validates();
                }
                catch (...) {
                    ss << "\n    exception firstextent\n" << endl;
                }

                set<DiskLoc> recs;
                if( scanData ) {
                    auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
                    int n = 0;
                    long long len = 0;
                    long long nlen = 0;
                    int outOfOrder = 0;
                    DiskLoc cl_last;
                    while ( c->ok() ) {
                        n++;

                        DiskLoc cl = c->currLoc();
                        if ( n < 1000000 )
                            recs.insert(cl);
                        if ( d->capped ) {
                            if ( cl < cl_last )
                                outOfOrder++;
                            cl_last = cl;
                        }

                        Record *r = c->_current();
                        len += r->lengthWithHeaders;
                        nlen += r->netLength();
                        c->advance();
                    }
                    if ( d->capped ) {
                        ss << "  capped outOfOrder:" << outOfOrder;
                        if ( outOfOrder > 1 ) {
                            valid = false;
                            ss << " ???";
                        }
                        else ss << " (OK)";
                        ss << '\n';
                    }
                    ss << "  " << n << " objects found, nobj:" << d->nrecords << "\n";
                    ss << "  " << len << " bytes data w/headers\n";
                    ss << "  " << nlen << " bytes data wout/headers\n";
                }

                ss << "  deletedList: ";
                for ( int i = 0; i < Buckets; i++ ) {
                    ss << (d->deletedList[i].isNull() ? '0' : '1');
                }
                ss << endl;
                int ndel = 0;
                long long delSize = 0;
                int incorrect = 0;
                for ( int i = 0; i < Buckets; i++ ) {
                    DiskLoc loc = d->deletedList[i];
                    try {
                        int k = 0;
                        while ( !loc.isNull() ) {
                            if ( recs.count(loc) )
                                incorrect++;
                            ndel++;

                            if ( loc.questionable() ) {
                                if ( loc.a() <= 0 || strstr(ns, "hudsonSmall") == 0 ) {
                                    ss << "    ?bad deleted loc: " << loc.toString() << " bucket:" << i << " k:" << k << endl;
                                    valid = false;
                                    break;
                                }
                            }

                            DeletedRecord *d = loc.drec();
                            delSize += d->lengthWithHeaders;
                            loc = d->nextDeleted;
                            k++;
                            checkForInterrupt();
                        }
                    } catch (...) {
                        ss <<"    ?exception in deleted chain for bucket " << i << endl;
                        valid = false;
                    }
                }
                ss << "  deleted: n: " << ndel << " size: " << delSize << endl;
                if ( incorrect ) {
                    ss << "    ?corrupt: " << incorrect << " records from datafile are in deleted list\n";
                    valid = false;
                }

                int idxn = 0;
                try  {
                    ss << "  nIndexes:" << d->nIndexes << endl;
                    NamespaceDetails::IndexIterator i = d->ii();
                    while( i.more() ) {
                        IndexDetails& id = i.next();
                        ss << "    " << id.indexNamespace() << " keys:" <<
                            id.head.btree()->fullValidate(id.head, id.keyPattern()) << endl;
                    }
                }
                catch (...) {
                    ss << "\n    exception during index validate idxn:" << idxn << endl;
                    valid=false;
                }

            }
            catch (AssertionException) {
                ss << "\n    exception during validate\n" << endl;
                valid = false;
            }

            if ( !valid )
                ss << " ns corrupt, requires dbchk\n";

            return ss.str();
        }
    } validateCmd;

    class FSyncCommand : public Command {
    public:
        FSyncCommand() : Command( "fsync" ){}

        virtual bool slaveOk(){ return true; }
        virtual bool adminOnly(){ return true; }
        
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl){
            bool sync = ! cmdObj["async"].trueValue();
            log() << "CMD fsync:  sync:" << sync << endl;
            result.append( "numFiles" , MemoryMappedFile::flushAll( sync ) );
            return 1;
        }
        
    } fsyncCmd;
    
}

