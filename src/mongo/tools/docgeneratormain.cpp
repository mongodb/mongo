/** @file docgeneratormain.cpp

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
*   Uses the DocumentGenerator class to populate databases with documents of the format
*   { id:xxx, counterUp:xxx, hashIdUp: hashof(counterUp), blob: xxx, counterDown:xxx,
*     hashIdUp: hashof(counterDown) }
*   The document size is 176 bytes.
*/

#include <iostream>

#include <boost/program_options.hpp>

#include "mongo/util/assert_util.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/tools/docgenerator.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongo;
using std::exception;
using std::cout;

namespace po = boost::program_options;

//------------- Define globals and constants--------

// global options object
DocGeneratorOptions globalDocGenOption;
// size of a sample document in bytes
const double documentSize = 176.0;

//----------- End globals and constants------------

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

        // Parse the values if supplied by the user. No data sanity check is performed
        // here so meaningless values may result in unexpected behavior.
        // TODO: Perform data sanity check
        if ( params.count("help") ) {
            cout << general_options << "\n";
            return 1;
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
            string ns = mongoutils::str::stream() << globalDocGenOption.prefix << i << ".sampledata";
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
