/** @file docgenerator.cpp

*    Copyright (C) 2012 10gen Inc.
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
*   This is a simple document generator. It generates documents in the format
*   { id:xxx, counterUp:xxx, hashIdUp: hashof(counterUp), blob: xxx, counterDown:xxx,
*     hashIdUp: hashof(counterDown) }
*   The document size is 176 bytes.
*/

#include <iostream>
#include <limits>

#include <boost/program_options.hpp>

#include "mongo/client/dbclientcursor.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/assert_util.h"

using namespace std;
using namespace mongo;
namespace po = boost::program_options;


struct DocGeneratorOptions {

    DocGeneratorOptions() :
        hostname(""),
        dbSize( 0.0 ),
        numdbs( 0 ),
        prefix("")
     { }
    string hostname;
    double dbSize;
    int numdbs;
    string prefix;
};

struct DocConfig {
    DocConfig() :
        counterUp( 0 ),
        counterDown( numeric_limits<long long>::max() ),
        blob( "" ),
        md5seed( "" )
    { }
    long long counterUp;
    long long counterDown;
    string blob;
    string md5seed;
};


//---------------------- Define globals and constants---------------------------

// global options object
DocGeneratorOptions globalDocGenOption;
// size of a sample document in bytes
const double documentSize = 176.0;


// Creates a documentGenerator that can be used to create sample documents

class DocumentGenerator {
    public:
    DocumentGenerator( ) { }
    ~DocumentGenerator() { }

    void init( BSONObj& args );
    BSONObj createDocument();

    // caller is responsible for managing this raw pointer
    static DocumentGenerator* makeDocumentGenerator( BSONObj args ) {
        DocumentGenerator* runner =  new DocumentGenerator() ;
        runner->init( args );
        return runner;
    }
    DocConfig config;
};


void DocumentGenerator::init( BSONObj& args ) {
    uassert( 16177, "blob is not a string", (args["blob"].type() == String) );
    config.blob = args["blob"].String();

    uassert( 16178, "md5 seed is not a string", (args["md5seed"].type() == String) );
    config.md5seed = args["md5seed"].String();

    uassert( 16179, "counterUp is not a number", args["counterUp"].isNumber() );
    config.counterUp = args["counterUp"].numberLong();

    uassert( 16180, "counterDown is not a number", args["counterDown"].isNumber() );
    config.counterDown = args["counterDown"].numberLong();
}

BSONObj DocumentGenerator::createDocument() {
    BSONObjBuilder doc;
    doc.genOID();

    doc.append( "counterUp" , config.counterUp );
    string hashUp = md5simpledigest( str::stream() << config.md5seed <<  config.counterUp );
    hashUp = hashUp.substr( 0, 8 );
    doc.append( "hashIdUp", atoll(hashUp.c_str()) );
    config.counterUp++;

    doc.append( "blobData" , config.blob );

    doc.append( "counterDown" , config.counterDown );
    string hashDown = md5simpledigest( str::stream() << config.md5seed <<  config.counterDown );
    hashDown = hashDown.substr( 0, 16 );
    doc.append( "hashIdDown", atoll(hashDown.c_str()) );

    config.counterDown--;

    return doc.obj();
}

int parseCmdLineOptions( int argc, char **argv ) {

    try {

        po::options_description general_options( "General options" );

        general_options.add_options()
        ( "help", "produce help message" )
        ( "hostname,H", po::value<string>() , "ip address of the host where mongod is running " )
        ( "dbSize", po::value<double>(), "size of each database in megabytes(MB)" )
        ( "numdbs", po::value<int>(), "number of databases you want" )
        ( "prefix", po::value<string>(),
                    "prefix the resultant db name where the documents "
                    "will be saved. DB's will be named prefix0, prefix1 etc." )
        ;

        po::variables_map params;
        po::store( po::parse_command_line(argc, argv, general_options), params );
        po::notify( params );

        /*
         * Parse the values if supplied by the user. No data sanity check is performed
         * here so meaningless values may result in unexpected behavior.
         * TODO: Perform data sanity check
         */

        if ( params.count("help") ) {
            cout << general_options << "\n";
            return 0;
        }
        if ( params.count("hostname") ) {
            globalDocGenOption.hostname = params["hostname"].as<string>();
        }
        if ( params.count("dbSize") ) {
            globalDocGenOption.dbSize = params["dbSize"].as<double>();
        }
        if ( params.count("numdbs") ) {
            globalDocGenOption.numdbs = params["numdbs"].as<int>();
        }
        if ( params.count("prefix") ) {
            globalDocGenOption.prefix = params["prefix"].as<string>();
        }
    }
    catch(exception& e) {
        cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}


int main( int argc, char* argv[] ) {

    if( parseCmdLineOptions( argc, argv) )
        return 1;

    // create a config object
    BSONObj args = BSONObjBuilder()
                   .append( "blob", "MongoDB is an open source document-oriented database system." )
                   .append( "md5seed", "newyork" )
                   .append( "counterUp", 0 )
                   .append( "counterDown", numeric_limits<long long>::max() ).obj();

    const int numDocsPerDB =
            static_cast<int>( globalDocGenOption.dbSize * 1024 * 1024 / documentSize );
    try {
        DBClientConnection conn;
        conn.connect( globalDocGenOption.hostname );
        cout << "successfully connected to the host" << endl;
        for( int i=0; i < globalDocGenOption.numdbs; ++i ) {
            scoped_ptr<DocumentGenerator> docGen( DocumentGenerator::makeDocumentGenerator(args) );
            cout << "populating database " << globalDocGenOption.prefix << i  << endl;
            long long j = 0;
            string ns = str::stream() << globalDocGenOption.prefix << i << ".sampledata";
            while( j != numDocsPerDB ) {
                BSONObj doc = docGen->createDocument();
                conn.insert( ns, doc );
                ++j;
            }
        }
    } catch( DBException &e ) {
        cout << "caught " << e.what() << endl;
    }
    return 0;
}
