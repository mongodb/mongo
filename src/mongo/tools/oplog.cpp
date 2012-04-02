// oplog.cpp

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

#include "pch.h"
#include "db/json.h"
#include "db/oplogreader.h"

#include "tool.h"

#include <fstream>
#include <iostream>

#include <boost/program_options.hpp>

using namespace mongo;

namespace po = boost::program_options;

class OplogTool : public Tool {
public:
    OplogTool() : Tool( "oplog" ) {
        addFieldOptions();
        add_options()
        ("seconds,s" , po::value<int>() , "seconds to go back default:86400" )
        ("from", po::value<string>() , "host to pull from" )
        ("oplogns", po::value<string>()->default_value( "local.oplog.rs" ) , "ns to pull from" )
        ;
    }

    virtual void printExtraHelp(ostream& out) {
        out << "Pull and replay a remote MongoDB oplog.\n" << endl;
    }

    int run() {

        if ( ! hasParam( "from" ) ) {
            log() << "need to specify --from" << endl;
            return -1;
        }

        Client::initThread( "oplogreplay" );

        log() << "going to connect" << endl;
        
        OplogReader r(false);
        r.setTailingQueryOptions( QueryOption_SlaveOk | QueryOption_AwaitData );
        r.connect( getParam( "from" ) );

        log() << "connected" << endl;

        OpTime start( time(0) - getParam( "seconds" , 86400 ) , 0 );
        log() << "starting from " << start.toStringPretty() << endl;

        string ns = getParam( "oplogns" );
        r.tailingQueryGTE( ns.c_str() , start );

        int num = 0;
        while ( r.more() ) {
            BSONObj o = r.next();
            LOG(2) << o << endl;
            
            if ( o["$err"].type() ) {
                log() << "error getting oplog" << endl;
                log() << o << endl;
                return -1;
            }
                

            bool print = ++num % 100000 == 0;
            if ( print )
                cout << num << "\t" << o << endl;
            
            if ( o["op"].String() == "n" )
                continue;

            if ( o["op"].String() == "c" ) {
                cout << "skipping: " << o << endl;
                continue;
            }

            BSONObjBuilder b( o.objsize() + 32 );
            BSONArrayBuilder updates( b.subarrayStart( "applyOps" ) );
            updates.append( o );
            updates.done();

            BSONObj c = b.obj();
            
            BSONObj res;
            bool ok = conn().runCommand( "admin" , c , res );
            if ( print || ! ok )
                log() << res << endl;
        }

        return 0;
    }
};

int main( int argc , char** argv ) {
    OplogTool t;
    return t.main( argc , argv );
}
