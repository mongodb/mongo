// dbcommands.cpp

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

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "json.h"
#include "repl.h"
#include "replset.h"
#include "commands.h"
#include "db.h"

extern int queryTraceLevel;
extern int otherTraceLevel;
extern int opLogging;
void flushOpLog();
int runCount(const char *ns, BSONObj& cmd, string& err);

void clean(const char *ns, NamespaceDetails *d) {
    for ( int i = 0; i < Buckets; i++ )
        d->deletedList[i].Null();
}

string validateNS(const char *ns, NamespaceDetails *d) {
    bool valid = true;
    stringstream ss;
    ss << "\nvalidate\n";
    ss << "  details: " << hex << d << " ofs:" << nsindex(ns)->detailsOffset(d) << dec << endl;
    if ( d->capped )
        ss << "  capped:" << d->capped << " max:" << d->max << '\n';

    ss << "  firstExtent:" << d->firstExtent.toString() << " ns:" << d->firstExtent.ext()->ns.buf << '\n';
    ss << "  lastExtent:" << d->lastExtent.toString()    << " ns:" << d->lastExtent.ext()->ns.buf << '\n';
    try {
        d->firstExtent.ext()->assertOk();
        d->lastExtent.ext()->assertOk();
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

        auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
        int n = 0;
        long long len = 0;
        long long nlen = 0;
        set<DiskLoc> recs;
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
                }
            } catch (...) {
                ss <<"    ?exception in deleted chain for bucket " << i << endl;
                valid = false;
            }
        }
        ss << "  deleted: n: " << ndel << " size: " << delSize << '\n';
        if ( incorrect ) {
            ss << "    ?corrupt: " << incorrect << " records from datafile are in deleted list\n";
            valid = false;
        }

        int idxn = 0;
        try  {
            ss << "  nIndexes:" << d->nIndexes << endl;
            for ( ; idxn < d->nIndexes; idxn++ ) {
                ss << "    " << d->indexes[idxn].indexNamespace() << " keys:" <<
                d->indexes[idxn].head.btree()->fullValidate(d->indexes[idxn].head) << endl;
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

class CmdDropDatabase : public Command {
public:
    virtual bool logTheOp() {
        return true;
    }
    virtual bool slaveOk() {
        return false;
    }
    CmdDropDatabase() : Command("dropDatabase") {}
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        BSONElement e = cmdObj.findElement(name);
        log() << "dropDatabase " << ns << endl;
        int p = (int) e.number();
        if ( p != 1 )
            return false;
        dropDatabase(ns);
        return true;
    }
} cmdDropDatabase;

class CmdRepairDatabase : public Command {
public:
    virtual bool logTheOp() {
        return false;
    }
    virtual bool slaveOk() {
        return true;
    }
    CmdRepairDatabase() : Command("repairDatabase") {}
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        BSONElement e = cmdObj.findElement(name);
        log() << "repairDatabase " << ns << endl;
        int p = (int) e.number();
        if ( p != 1 )
            return false;
        e = cmdObj.findElement( "preserveClonedFilesOnFailure" );
        bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
        e = cmdObj.findElement( "backupOriginalFiles" );
        bool backupOriginalFiles = e.isBoolean() && e.boolean();
        return repairDatabase( ns, errmsg, preserveClonedFilesOnFailure, backupOriginalFiles );
    }
} cmdRepairDatabase;

/* set db profiling level
   todo: how do we handle profiling information put in the db with replication?
         sensibly or not?
*/
class CmdProfile : public Command {
public:
    virtual bool slaveOk() {
        return true;
    }
    CmdProfile() : Command("profile") {}
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        BSONElement e = cmdObj.findElement(name);
        result.append("was", (double) database->profile);
        int p = (int) e.number();
        bool ok = false;
        if ( p == -1 )
            ok = true;
        else if ( p >= 0 && p <= 2 ) {
            ok = true;
            database->profile = p;
        }
        return ok;
    }
} cmdProfile;

class CmdTimeInfo : public Command {
public:
    virtual bool slaveOk() {
        return true;
    }
    CmdTimeInfo() : Command("timeinfo") {}
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        unsigned long long last, start, timeLocked;
        dbMutexInfo.timingInfo(start, timeLocked);
        last = curTimeMicros64();
        double tt = (double) last-start;
        double tl = (double) timeLocked;
        result.append("totalTime", tt);
        result.append("lockTime", tl);
        result.append("ratio", tl/tt);
        return true;
    }
} cmdTimeInfo;

/* just to check if the db has asserted */
class CmdAssertInfo : public Command {
public:
    virtual bool slaveOk() {
        return true;
    }
    CmdAssertInfo() : Command("assertinfo") {}
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        result.appendBool("dbasserted", lastAssert[0].isSet() || lastAssert[1].isSet() || lastAssert[2].isSet());
        result.appendBool("asserted", lastAssert[0].isSet() || lastAssert[1].isSet() || lastAssert[2].isSet() || lastAssert[3].isSet());
        result.append("assert", lastAssert[AssertRegular].toString());
        result.append("assertw", lastAssert[AssertW].toString());
        result.append("assertmsg", lastAssert[AssertMsg].toString());
        result.append("assertuser", lastAssert[AssertUser].toString());
        return true;
    }
} cmdAsserts;

class CmdGetOpTime : public Command {
public:
    virtual bool slaveOk() {
        return true;
    }
    CmdGetOpTime() : Command("getoptime") { }
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        result.appendDate("optime", OpTime::now().asDate());
        return true;
    }
} cmdgetoptime;

/*
class Cmd : public Command {
public:
    Cmd() : Command("") { }
    bool adminOnly() { return true; }
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result) {
        return true;
    }
} cmd;
*/

class CmdOpLogging : public Command {
public:
    virtual bool slaveOk() {
        return true;
    }
    CmdOpLogging() : Command("opLogging") { }
    bool adminOnly() {
        return true;
    }
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
        opLogging = (int) cmdObj.findElement(name).number();
        flushOpLog();
        log() << "CMD: opLogging set to " << opLogging << endl;
        return true;
    }
} cmdoplogging;

/* drop collection */
class CmdDrop : public Command {
public:
    CmdDrop() : Command("drop") { }
    virtual bool logTheOp() {
        return true;
    }
    virtual bool slaveOk() {
        return false;
    }
    virtual bool adminOnly() {
        return false;
    }
    virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
        string nsToDrop = database->name + '.' + cmdObj.findElement(name).valuestr();
        NamespaceDetails *d = nsdetails(nsToDrop.c_str());
        log() << "CMD: drop " << nsToDrop << endl;
        if ( d == 0 ) {
            errmsg = "ns not found";
            return false;
        }
        if ( d->nIndexes != 0 ) {
            // client helper function is supposed to drop the indexes first
            errmsg = "ns has indexes (not permitted on drop)";
            return false;
        }
        result.append("ns", nsToDrop.c_str());
        ClientCursor::invalidate(nsToDrop.c_str());
        dropNS(nsToDrop);
        return true;
    }
} cmdDrop;

class CmdQueryTraceLevel : public Command {
public:
    virtual bool slaveOk() {
        return true;
    }
    CmdQueryTraceLevel() : Command("queryTraceLevel") { }
    bool adminOnly() {
        return true;
    }
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
        queryTraceLevel = (int) cmdObj.findElement(name).number();
        return true;
    }
} cmdquerytracelevel;

class CmdTraceAll : public Command {
public:
    virtual bool slaveOk() {
        return true;
    }
    CmdTraceAll() : Command("traceAll") { }
    bool adminOnly() {
        return true;
    }
    bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
        queryTraceLevel = otherTraceLevel = (int) cmdObj.findElement(name).number();
        return true;
    }
} cmdtraceall;

/* select count(*) */
class CmdCount : public Command {
public:
    CmdCount() : Command("count") { }
    virtual bool logTheOp() {
        return false;
    }
    virtual bool slaveOk() {
        return false;
    }
    virtual bool adminOnly() {
        return false;
    }
    virtual bool run(const char *_ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
        string ns = database->name + '.' + cmdObj.findElement(name).valuestr();
        string err;
        int n = runCount(ns.c_str(), cmdObj, err);
        int nn = n;
        bool ok = true;
        if ( n < 0 ) {
            ok = false;
            nn = 0;
            if ( !err.empty() )
                errmsg = err;
        }
        result.append("n", (double) nn);
        return ok;
    }
} cmdCount;

/* create collection */
class CmdCreate : public Command {
public:
    CmdCreate() : Command("create") { }
    virtual bool logTheOp() {
        return true;
    }
    virtual bool slaveOk() {
        return false;
    }
    virtual bool adminOnly() {
        return false;
    }
    virtual bool run(const char *_ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
        string ns = database->name + '.' + cmdObj.findElement(name).valuestr();
        string err;
        bool ok = userCreateNS(ns.c_str(), cmdObj, err, true);
        if ( !ok && !err.empty() )
            errmsg = err;
        return ok;
    }
} cmdCreate;

class CmdDeleteIndexes : public Command {
public:
    virtual bool logTheOp() {
        return true;
    }
    virtual bool slaveOk() {
        return false;
    }
    CmdDeleteIndexes() : Command("deleteIndexes") { }
    bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& anObjBuilder, bool /*fromRepl*/) {
        /* note: temp implementation.  space not reclaimed! */
        BSONElement e = jsobj.findElement(name.c_str());
        string toDeleteNs = database->name + '.' + e.valuestr();
        NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
        log() << "CMD: deleteIndexes " << toDeleteNs << endl;
        if ( d ) {
            BSONElement f = jsobj.findElement("index");
            if ( !f.eoo() ) {

                d->aboutToDeleteAnIndex();

                ClientCursor::invalidate(toDeleteNs.c_str());

                // delete a specific index or all?
                if ( f.type() == String ) {
                    const char *idxName = f.valuestr();
                    if ( *idxName == '*' && idxName[1] == 0 ) {
                        log() << "  d->nIndexes was " << d->nIndexes << '\n';
                        anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
                        anObjBuilder.append("msg", "all indexes deleted for collection");
                        for ( int i = 0; i < d->nIndexes; i++ )
                            d->indexes[i].kill();
                        d->nIndexes = 0;
                        log() << "  alpha implementation, space not reclaimed" << endl;
                    }
                    else {
                        // delete just one index
                        int x = d->findIndexByName(idxName);
                        if ( x >= 0 ) {
                            cout << "  d->nIndexes was " << d->nIndexes << endl;
                            anObjBuilder.append("nIndexesWas", (double)d->nIndexes);

                            /* note it is  important we remove the IndexDetails with this
                            call, otherwise, on recreate, the old one would be reused, and its
                            IndexDetails::info ptr would be bad info.
                            */
                            d->indexes[x].kill();

                            d->nIndexes--;
                            for ( int i = x; i < d->nIndexes; i++ )
                                d->indexes[i] = d->indexes[i+1];
                            log() << "deleteIndexes: alpha implementation, space not reclaimed\n";
                        } else {
                            log() << "deleteIndexes: " << idxName << " not found" << endl;
                            errmsg = "index not found";
                            return false;
                        }
                    }
                }
            }
        }
        else {
            errmsg = "ns not found";
            return false;
        }
        return true;
    }
} cmdDeleteIndexes;

class CmdListDatabases : public Command {
public:
    virtual bool logTheOp() { return false; }
    virtual bool slaveOk() { return true; }
    virtual bool adminOnly() { return true; }
    CmdListDatabases() : Command("listDatabases") {}
    bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
        vector< string > dbNames;
        getDatabaseNames( dbNames );
        vector< BSONObj > dbInfos;
        for( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
            BSONObjBuilder s;
            s.append( "diskSize", dbSize( i->c_str() ) );
            BSONObjBuilder b;
            b.append( i->c_str(), s.done() );
            dbInfos.push_back( b.doneAndDecouple() );
        }
        result.append( "databases", dbInfos );
        return true;
    }
} cmdListDatabases;

extern map<string,Command*> *commands;

/* TODO make these all command objects -- legacy stuff here

   usage:
     abc.$cmd.findOne( { ismaster:1 } );

   returns true if ran a cmd
*/
bool _runCommands(const char *ns, BSONObj& _cmdobj, stringstream& ss, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl) {
    const char *p = strchr(ns, '.');
    if ( !p ) return false;
    if ( strcmp(p, ".$cmd") != 0 ) return false;

    BSONObj jsobj;
    {
        BSONElement e = _cmdobj.firstElement();
        if ( e.type() == Object && string("query") == e.fieldName() ) {
            jsobj = e.embeddedObject();
        }
        else {
            jsobj = _cmdobj;
        }
    }

    bool ok = false;
    bool valid = false;

    BSONElement e;
    e = jsobj.firstElement();

    map<string,Command*>::iterator i;

    if ( e.eoo() )
        ;
    /* check for properly registered command objects.  Note that all the commands below should be
       migrated over to the command object format.
       */
    else if ( (i = commands->find(e.fieldName())) != commands->end() ) {
        valid = true;
        string errmsg;
        Command *c = i->second;
        if ( c->adminOnly() && !fromRepl && strncmp(ns, "admin", p-ns) != 0 ) {
            ok = false;
            errmsg = "access denied";
        }
        else if ( !isMaster() && !c->slaveOk() && !fromRepl ) {
            /* todo: allow if Option_SlaveOk was set on the query */
            ok = false;
            errmsg = "not master";
        }
        else {
            ok = c->run(ns, jsobj, errmsg, anObjBuilder, fromRepl);
            if ( ok && c->logTheOp() && !fromRepl )
                logOp("c", ns, jsobj);
        }
        if ( !ok )
            anObjBuilder.append("errmsg", errmsg);
    }
    else if ( e.type() == String ) {
        /* { count: "collectionname"[, query: <query>] } */
        string us(ns, p-ns);

        /* we allow clean and validate on slaves */
        if ( strcmp( e.fieldName(), "clean") == 0 ) {
            valid = true;
            string dropNs = us + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(dropNs.c_str());
            log() << "CMD: clean " << dropNs << endl;
            if ( d ) {
                ok = true;
                anObjBuilder.append("ns", dropNs.c_str());
                clean(dropNs.c_str(), d);
            }
            else {
                anObjBuilder.append("errmsg", "ns not found");
            }
        }
        else if ( strcmp( e.fieldName(), "validate") == 0 ) {
            valid = true;
            string toValidateNs = us + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toValidateNs.c_str());
            log() << "CMD: validate " << toValidateNs << endl;
            if ( d ) {
                ok = true;
                anObjBuilder.append("ns", toValidateNs.c_str());
                string s = validateNS(toValidateNs.c_str(), d);
                anObjBuilder.append("result", s.c_str());
            }
            else {
                anObjBuilder.append("errmsg", "ns not found");
            }
        }
    }

    if ( !valid )
        anObjBuilder.append("errmsg", "no such cmd");
    anObjBuilder.append("ok", ok?1.0:0.0);
    BSONObj x = anObjBuilder.done();
    b.append((void*) x.objdata(), x.objsize());
    return true;
}

