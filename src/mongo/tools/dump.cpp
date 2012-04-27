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
#include "../db/db.h"
#include "mongo/client/dbclientcursor.h"
#include "tool.h"

#include <fcntl.h>
#include <map>
#include <fstream>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>

using namespace mongo;

namespace po = boost::program_options;

class Dump : public Tool {
    class FilePtr : boost::noncopyable {
    public:
        /*implicit*/ FilePtr(FILE* f) : _f(f) {}
        ~FilePtr() { fclose(_f); }
        operator FILE*() { return _f; }
    private:
        FILE* _f;
    };
public:
    Dump() : Tool( "dump" , ALL , "" , "" , true ) {
        add_options()
        ("out,o", po::value<string>()->default_value("dump"), "output directory or \"-\" for stdout")
        ("query,q", po::value<string>() , "json query" )
        ("oplog", "Use oplog for point-in-time snapshotting" )
        ("repair", "try to recover a crashed database" )
        ("forceTableScan", "force a table scan (do not use $snapshot)" )
        ;
    }

    virtual void preSetup() {
        string out = getParam("out");
        if ( out == "-" ) {
                // write output to standard error to avoid mangling output
                // must happen early to avoid sending junk to stdout
                useStandardOutput(false);
        }
    }

    virtual void printExtraHelp(ostream& out) {
        out << "Export MongoDB data to BSON files.\n" << endl;
    }

    // This is a functor that writes a BSONObj to a file
    struct Writer {
        Writer(FILE* out, ProgressMeter* m) :_out(out), _m(m) {}

        void operator () (const BSONObj& obj) {
            size_t toWrite = obj.objsize();
            size_t written = 0;

            while (toWrite) {
                size_t ret = fwrite( obj.objdata()+written, 1, toWrite, _out );
                uassert(14035, errnoWithPrefix("couldn't write to file"), ret);
                toWrite -= ret;
                written += ret;
            }

            // if there's a progress bar, hit it
            if (_m) {
                _m->hit();
            }
        }

        FILE* _out;
        ProgressMeter* _m;
    };

    void doCollection( const string coll , FILE* out , ProgressMeter *m ) {
        Query q = _query;

        int queryOptions = QueryOption_SlaveOk | QueryOption_NoCursorTimeout;
        if (startsWith(coll.c_str(), "local.oplog."))
            queryOptions |= QueryOption_OplogReplay;
        else if ( _query.isEmpty() && !hasParam("dbpath") && !hasParam("forceTableScan") )
            q.snapshot();
        
        DBClientBase& connBase = conn(true);
        Writer writer(out, m);

        // use low-latency "exhaust" mode if going over the network
        if (!_usingMongos && typeid(connBase) == typeid(DBClientConnection&)) {
            DBClientConnection& conn = static_cast<DBClientConnection&>(connBase);
            boost::function<void(const BSONObj&)> castedWriter(writer); // needed for overload resolution
            conn.query( castedWriter, coll.c_str() , q , NULL, queryOptions | QueryOption_Exhaust);
        }
        else {
            //This branch should only be taken with DBDirectClient or mongos which doesn't support exhaust mode
            scoped_ptr<DBClientCursor> cursor(connBase.query( coll.c_str() , q , 0 , 0 , 0 , queryOptions ));
            while ( cursor->more() ) {
                writer(cursor->next());
            }
        }
    }

    void writeCollectionFile( const string coll , boost::filesystem::path outputFile ) {
        log() << "\t" << coll << " to " << outputFile.string() << endl;

        FilePtr f (fopen(outputFile.string().c_str(), "wb"));
        uassert(10262, errnoWithPrefix("couldn't open file"), f);

        ProgressMeter m( conn( true ).count( coll.c_str() , BSONObj() , QueryOption_SlaveOk ) );
        m.setUnits("objects");

        doCollection(coll, f, &m);

        log() << "\t\t " << m.done() << " objects" << endl;
    }

    void writeMetadataFile( const string coll, boost::filesystem::path outputFile, 
                            map<string, BSONObj> options, multimap<string, BSONObj> indexes ) {
        log() << "\tMetadata for " << coll << " to " << outputFile.string() << endl;

        ofstream file (outputFile.string().c_str());
        uassert(15933, "Couldn't open file: " + outputFile.string(), file.is_open());

        bool hasOptions = options.count(coll) > 0;
        bool hasIndexes = indexes.count(coll) > 0;

        if (hasOptions) {
            file << "{options : " << options.find(coll)->second.jsonString();

            if (hasIndexes) {
                file << ", ";
            }
        } else {
            file << "{";
        }

        if (hasIndexes) {
            file << "indexes:[";
            for (multimap<string, BSONObj>::iterator it=indexes.equal_range(coll).first; it!=indexes.equal_range(coll).second; ++it) {
                if (it != indexes.equal_range(coll).first) {
                    file << ", ";
                }
                file << (*it).second.jsonString();
            }
            file << "]";
        }
        file << "}";
    }



    void writeCollectionStdout( const string coll ) {
        doCollection(coll, stdout, NULL);
    }

    void go( const string db , const boost::filesystem::path outdir ) {
        log() << "DATABASE: " << db << "\t to \t" << outdir.string() << endl;

        boost::filesystem::create_directories( outdir );

        map <string, BSONObj> collectionOptions;
        multimap <string, BSONObj> indexes;
        vector <string> collections;

        // Save indexes for database
        string ins = db + ".system.indexes";
        auto_ptr<DBClientCursor> cursor = conn( true ).query( ins.c_str() , Query() , 0 , 0 , 0 , QueryOption_SlaveOk | QueryOption_NoCursorTimeout );
        while ( cursor->more() ) {
            BSONObj obj = cursor->nextSafe();
            const string name = obj.getField( "ns" ).valuestr();
            indexes.insert( pair<string, BSONObj> (name, obj.getOwned()) );
        }

        string sns = db + ".system.namespaces";
        cursor = conn( true ).query( sns.c_str() , Query() , 0 , 0 , 0 , QueryOption_SlaveOk | QueryOption_NoCursorTimeout );
        while ( cursor->more() ) {
            BSONObj obj = cursor->nextSafe();
            const string name = obj.getField( "name" ).valuestr();
            if (obj.hasField("options")) {
                collectionOptions.insert( pair<string,BSONObj> (name, obj.getField("options").embeddedObject()) );
            }

            // skip namespaces with $ in them only if we don't specify a collection to dump
            if ( _coll == "" && name.find( ".$" ) != string::npos ) {
                log(1) << "\tskipping collection: " << name << endl;
                continue;
            }

            const string filename = name.substr( db.size() + 1 );

            //if a particular collections is specified, and it's not this one, skip it
            if ( _coll != "" && db + "." + _coll != name && _coll != name )
                continue;

            // raise error before writing collection with non-permitted filename chars in the name
            size_t hasBadChars = name.find_first_of("/\0");
            if (hasBadChars != string::npos){
              error() << "Cannot dump "  << name << ". Collection has '/' or null in the collection name." << endl;
              continue;
            }
            
            // Don't dump indexes
            if ( endsWith(name.c_str(), ".system.indexes") ) {
              continue;
            }
            
            if ( _coll != "" && db + "." + _coll != name && _coll != name )
              continue;
            
            collections.push_back(name);
        }
        
        for (vector<string>::iterator it = collections.begin(); it != collections.end(); ++it) {
            string name = *it;
            const string filename = name.substr( db.size() + 1 );
            writeCollectionFile( name , outdir / ( filename + ".bson" ) );
            writeMetadataFile( name, outdir / (filename + ".metadata.json"), collectionOptions, indexes);
        }

    }

    int repair() {
        if ( ! hasParam( "dbpath" ) ){
            log() << "repair mode only works with --dbpath" << endl;
            return -1;
        }
        
        if ( ! hasParam( "db" ) ){
            log() << "repair mode only works on 1 db right at a time right now" << endl;
            return -1;
        }

        string dbname = getParam( "db" );
        log() << "going to try and recover data from: " << dbname << endl;

        return _repair( dbname  );
    }
    
    DiskLoc _repairExtent( Database* db , string ns, bool forward , DiskLoc eLoc , Writer& w ){
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
        
        set<DiskLoc> seen;

        DiskLoc loc = forward ? e->firstRecord : e->lastRecord;
        while ( ! loc.isNull() ){
            
            if ( ! seen.insert( loc ).second ) {
                error() << "infinite loop in extend, seen: " << loc << " before" << endl;
                break;
            }

            if ( loc.getOfs() <= 0 ){
                error() << "offset is 0 for record which should be impossible" << endl;
                break;
            }
            log(1) << loc << endl;
            Record* rec = loc.rec();
            BSONObj obj;
            try {
                obj = loc.obj();
                verify( obj.valid() );
                LOG(1) << obj << endl;
                w( obj );
            }
            catch ( std::exception& e ) {
                log() << "found invalid document @ " << loc << " " << e.what() << endl;
                if ( ! obj.isEmpty() ) {
                    try {
                        BSONElement e = obj.firstElement();
                        stringstream ss;
                        ss << "first element: " << e;
                        log() << ss.str();
                    }
                    catch ( std::exception& ) {
                    }
                }
            }
            loc = forward ? rec->getNext( loc ) : rec->getPrev( loc );
        }
        return forward ? e->xnext : e->xprev;
        
    }

    void _repair( Database* db , string ns , boost::filesystem::path outfile ){
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

        outfile /= ( ns.substr( ns.find( "." ) + 1 ) + ".bson" );
        log() << "writing to: " << outfile.string() << endl;
        
        FilePtr f (fopen(outfile.string().c_str(), "wb"));

        ProgressMeter m( nsd->stats.nrecords * 2 );
        m.setUnits("objects");
        
        Writer w( f , &m );

        try {
            log() << "forward extent pass" << endl;
            LogIndentLevel lil;
            DiskLoc eLoc = nsd->firstExtent;
            while ( ! eLoc.isNull() ){
                log() << "extent loc: " << eLoc << endl;
                eLoc = _repairExtent( db , ns , true , eLoc , w );
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
                eLoc = _repairExtent( db , ns , false , eLoc , w );
            }
        }
        catch ( DBException& e ){
            error() << "ERROR: backwards extent pass failed:" << e.toString() << endl;
        }

        log() << "\t\t " << m.done() << " objects" << endl;
    }
    
    int _repair( string dbname ) {
        Client::WriteContext cx( dbname );
        Database * db = cx.ctx().db();
        
        list<string> namespaces;
        db->namespaceIndex.getNamespaces( namespaces );
        
        boost::filesystem::path root = getParam( "out" );
        root /= dbname;
        boost::filesystem::create_directories( root );

        for ( list<string>::iterator i=namespaces.begin(); i!=namespaces.end(); ++i ){
            LogIndentLevel lil;
            string ns = *i;

            if ( str::endsWith( ns , ".system.namespaces" ) )
                continue;
            
            if ( str::contains( ns , ".tmp.mr." ) )
                continue;
            
            if ( _coll != "" && ! str::endsWith( ns , _coll ) )
                continue;

            log() << "trying to recover: " << ns << endl;
            
            LogIndentLevel lil2;
            try {
                _repair( db , ns , root );
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
                log() << "oplog mode is only supported on full dumps" << endl;
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
                    log() << "oplog mode is only supported on master or replica set member" << endl;
                    return -1;
                }
            }

            auth("local");

            BSONObj op = conn(true).findOne(opLogName, Query().sort("$natural", -1), 0, QueryOption_SlaveOk);
            if (op.isEmpty()) {
                log() << "No operations in oplog. Please ensure you are connecting to a master." << endl;
                return -1;
            }

            verify(op["ts"].type() == Timestamp);
            opLogStart = op["ts"]._numberLong();
        }

        // check if we're outputting to stdout
        string out = getParam("out");
        if ( out == "-" ) {
            if ( _db != "" && _coll != "" ) {
                writeCollectionStdout( _db+"."+_coll );
                return 0;
            }
            else {
                log() << "You must specify database and collection to print to stdout" << endl;
                return -1;
            }
        }

        _usingMongos = isMongos();

        boost::filesystem::path root( out );
        string db = _db;

        if ( db == "" ) {
            log() << "all dbs" << endl;
            auth( "admin" );

            BSONObj res = conn( true ).findOne( "admin.$cmd" , BSON( "listDatabases" << 1 ) );
            if ( ! res["databases"].isABSONObj() ) {
                error() << "output of listDatabases isn't what we expected, no 'databases' field:\n" << res << endl;
                return -2;
            }
            BSONObj dbs = res["databases"].embeddedObjectUserCheck();
            set<string> keys;
            dbs.getFieldNames( keys );
            for ( set<string>::iterator i = keys.begin() ; i != keys.end() ; i++ ) {
                string key = *i;
                
                if ( ! dbs[key].isABSONObj() ) {
                    error() << "database field not an object key: " << key << " value: " << dbs[key] << endl;
                    return -3;
                }

                BSONObj dbobj = dbs[key].embeddedObjectUserCheck();

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

    bool _usingMongos;
    BSONObj _query;
};

int main( int argc , char ** argv ) {
    Dump d;
    return d.main( argc , argv );
}
