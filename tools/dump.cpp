// dump.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "../pch.h"
#include "../client/dbclient.h"
#include "tool.h"

#include <fcntl.h>

using namespace mongo;

namespace po = boost::program_options;

class Dump : public Tool {
public:
    Dump() : Tool( "dump" , ALL , "*" , "*" , false ) {
        add_options()
        ("out,o", po::value<string>()->default_value("dump"), "output directory or \"-\" for stdout")
        ("query,q", po::value<string>() , "json query" )
        ("oplog", "Use oplog for point-in-time snapshotting" )
        ("repair", "try to recover a crashed database" )
        ;
    }

    // This is a functor that writes a BSONObj to a file
    struct Writer {
        Writer(ostream& out, ProgressMeter* m) :_out(out), _m(m) {}

        void operator () (const BSONObj& obj) {
            _out.write( obj.objdata() , obj.objsize() );

            // if there's a progress bar, hit it
            if (_m) {
                _m->hit();
            }
        }

        ostream& _out;
        ProgressMeter* _m;
    };

    void doCollection( const string coll , ostream &out , ProgressMeter *m ) {
        Query q;
        if ( _query.isEmpty() && !hasParam("dbpath"))
            q.snapshot();
        else
            q = _query;

        int queryOptions = QueryOption_SlaveOk | QueryOption_NoCursorTimeout;
        if (startsWith(coll.c_str(), "local.oplog."))
            queryOptions |= QueryOption_OplogReplay;

        DBClientBase& connBase = conn(true);
        Writer writer(out, m);

        // use low-latency "exhaust" mode if going over the network
        if (typeid(connBase) == typeid(DBClientConnection&)) {
            DBClientConnection& conn = static_cast<DBClientConnection&>(connBase);
            boost::function<void(const BSONObj&)> castedWriter(writer); // needed for overload resolution
            conn.query( castedWriter, coll.c_str() , q , NULL, queryOptions | QueryOption_Exhaust);
        }
        else {
            //This branch should only be taken with DBDirectClient which doesn't support exhaust mode
            scoped_ptr<DBClientCursor> cursor(connBase.query( coll.c_str() , q , 0 , 0 , 0 , queryOptions ));
            while ( cursor->more() ) {
                writer(cursor->next());
            }
        }
    }

    void writeCollectionFile( const string coll , path outputFile ) {
        cout << "\t" << coll << " to " << outputFile.string() << endl;

        ofstream out;
        out.open( outputFile.string().c_str() , ios_base::out | ios_base::binary  );
        assertStreamGood( 10262 ,  "couldn't open file" , out );

        ProgressMeter m( conn( true ).count( coll.c_str() , BSONObj() , QueryOption_SlaveOk ) );

        doCollection(coll, out, &m);

        cout << "\t\t " << m.done() << " objects" << endl;

        out.close();
    }

    void writeCollectionStdout( const string coll ) {
        doCollection(coll, cout, NULL);
    }

    void go( const string db , const path outdir ) {
        cout << "DATABASE: " << db << "\t to \t" << outdir.string() << endl;

        create_directories( outdir );

        string sns = db + ".system.namespaces";

        auto_ptr<DBClientCursor> cursor = conn( true ).query( sns.c_str() , Query() , 0 , 0 , 0 , QueryOption_SlaveOk | QueryOption_NoCursorTimeout );
        while ( cursor->more() ) {
            BSONObj obj = cursor->nextSafe();
            if ( obj.toString().find( ".$" ) != string::npos )
                continue;

            const string name = obj.getField( "name" ).valuestr();
            const string filename = name.substr( db.size() + 1 );

            if ( _coll != "*" && db + "." + _coll != name && _coll != name )
                continue;

            writeCollectionFile( name.c_str() , outdir / ( filename + ".bson" ) );

        }

    }

    int repair() {
        if ( ! hasParam( "dbpath" ) ){
            cout << "repair mode only works with --dbpath" << endl;
            return -1;
        }
        
        if ( ! hasParam( "db" ) ){
            cout << "repair mode only works on 1 db right at a time right now" << endl;
            return -1;
        }

        if ( hasParam( "collection" ) ){
            cout << "repair mode can't work with collection, only on full db" << endl;
            return -1;
        }

        string dbname = getParam( "db" );
        log() << "going to try and recover data from: " << dbname << endl;

        return _repair( dbname  );
    }
    
    DiskLoc _repairExtent( Database* db , string ns, bool forward , DiskLoc eLoc ){
        LogIndentLevel lil;
        
        if ( eLoc.getOfs() <= 0 ){
            error() << "invalid extent ofs: " << eLoc.getOfs() << endl;
            return DiskLoc();
        }
        

        MongoDataFile * mdf = db->getFile( eLoc.a() );

        Extent * e = mdf->debug_getExtent( eLoc );
        if ( ! e->isOk() ){
            warning() << "Extent not ok magic: " << e->magic << " going to try to continue" << endl;
        }
        
        log() << "length:" << e->length << endl;
        
        LogIndentLevel lil2;
        
        DiskLoc loc = forward ? e->firstRecord : e->lastRecord;
        while ( ! loc.isNull() ){
            if ( loc.getOfs() <= 0 ){
                error() << "offset is 0 for record which should be impossible" << endl;
                break;
            }
            log() << loc << endl;
            Record* rec = loc.rec();
            log() << loc.obj() << endl;
            loc = forward ? rec->getNext( loc ) : rec->getPrev( loc );
        }
        return forward ? e->xnext : e->xprev;
        
    }

    void _repair( Database* db , string ns ){
        NamespaceDetails * nsd = nsdetails( ns.c_str() );
        log() << "nrecords: " << nsd->stats.nrecords 
              << " datasize: " << nsd->stats.datasize 
              << " firstExtent: " << nsd->firstExtent 
              << endl;
        
        if ( nsd->firstExtent.isNull() ){
            log() << " ERROR fisrtExtent is null" << endl;
            return;
        }
        
        if ( ! nsd->firstExtent.isValid() ){
            log() << " ERROR fisrtExtent is not valid" << endl;
            return;
        }
        
        try {
            log() << "forward extent pass" << endl;
            LogIndentLevel lil;
            DiskLoc eLoc = nsd->firstExtent;
            while ( ! eLoc.isNull() ){
                log() << "extent loc: " << eLoc << endl;
                eLoc = _repairExtent( db , ns , true , eLoc );
            }
        }
        catch ( DBException& e ){
            error() << "forward extent pass failed:" << e.toString() << endl;
        }

        try {
            log() << "backwards extent pass" << endl;
            LogIndentLevel lil;
            DiskLoc eLoc = nsd->lastExtent;
            while ( ! eLoc.isNull() ){
                log() << "extent loc: " << eLoc << endl;
                eLoc = _repairExtent( db , ns , false , eLoc );
            }
        }
        catch ( DBException& e ){
            error() << "ERROR: backwards extent pass failed:" << e.toString() << endl;
        }

    }
    
    int _repair( string dbname ){
        dblock lk;
        Client::Context cx( dbname );
        Database * db = cx.db();
        
        list<string> namespaces;
        db->namespaceIndex.getNamespaces( namespaces );
        
        for ( list<string>::iterator i=namespaces.begin(); i!=namespaces.end(); ++i ){
            LogIndentLevel lil;
            string ns = *i;
            if ( str::endsWith( ns , ".system.namespaces" ) )
                continue;
            log() << "trying to recover: " << ns << endl;
            
            LogIndentLevel lil2;
            try {
                _repair( db , ns );
            }
            catch ( DBException& e ){
                log() << "ERROR recovering: " << ns << " " << e.toString() << endl;
            }
        }
   
        return 0;
    }

    int run() {
        
        if ( hasParam( "repair" ) ){
            warning() << "repair is a work in progress" << endl;
            return repair();
        }

        {
            string q = getParam("query");
            if ( q.size() )
                _query = fromjson( q );
        }

        string opLogName = "";
        unsigned long long opLogStart = 0;
        if (hasParam("oplog")) {
            if (hasParam("query") || hasParam("db") || hasParam("collection")) {
                cout << "oplog mode is only supported on full dumps" << endl;
                return -1;
            }


            BSONObj isMaster;
            conn("true").simpleCommand("admin", &isMaster, "isMaster");

            if (isMaster.hasField("hosts")) { // if connected to replica set member
                opLogName = "local.oplog.rs";
            }
            else {
                opLogName = "local.oplog.$main";
                if ( ! isMaster["ismaster"].trueValue() ) {
                    cout << "oplog mode is only supported on master or replica set member" << endl;
                    return -1;
                }
            }

            BSONObj op = conn(true).findOne(opLogName, Query().sort("$natural", -1), 0, QueryOption_SlaveOk);
            if (op.isEmpty()) {
                cout << "No operations in oplog. Please ensure you are connecting to a master." << endl;
                return -1;
            }

            assert(op["ts"].type() == Timestamp);
            opLogStart = op["ts"]._numberLong();
        }



        // check if we're outputting to stdout
        string out = getParam("out");
        if ( out == "-" ) {
            if ( _db != "*" && _coll != "*" ) {
                writeCollectionStdout( _db+"."+_coll );
                return 0;
            }
            else {
                cout << "You must specify database and collection to print to stdout" << endl;
                return -1;
            }
        }

        path root( out );
        string db = _db;

        if ( db == "*" ) {
            cout << "all dbs" << endl;
            auth( "admin" );

            BSONObj res = conn( true ).findOne( "admin.$cmd" , BSON( "listDatabases" << 1 ) );
            BSONObj dbs = res.getField( "databases" ).embeddedObjectUserCheck();
            set<string> keys;
            dbs.getFieldNames( keys );
            for ( set<string>::iterator i = keys.begin() ; i != keys.end() ; i++ ) {
                string key = *i;

                BSONObj dbobj = dbs.getField( key ).embeddedObjectUserCheck();

                const char * dbName = dbobj.getField( "name" ).valuestr();
                if ( (string)dbName == "local" )
                    continue;

                go ( dbName , root / dbName );
            }
        }
        else {
            auth( db );
            go( db , root / db );
        }

        if (!opLogName.empty()) {
            BSONObjBuilder b;
            b.appendTimestamp("$gt", opLogStart);

            _query = BSON("ts" << b.obj());

            writeCollectionFile( opLogName , root / "oplog.bson" );
        }

        return 0;
    }

    BSONObj _query;
};

int main( int argc , char ** argv ) {
    Dump d;
    return d.main( argc , argv );
}
