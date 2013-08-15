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

#include "mongo/pch.h"

#include <fstream>
#include <iostream>

#include "mongo/base/init.h"
#include "mongo/db/json.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/tools/tool.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

using namespace mongo;

namespace mongo {
    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                              MONGO_NO_PREREQUISITES,
                              ("default"))(InitializerContext* context) {

        options = moe::OptionSection( "options" );
        moe::OptionsParser parser;

        Status retStatus = addMongoOplogOptions(&options);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = parser.run(options, context->args(), context->env(), &_params);
        if (!retStatus.isOK()) {
            std::ostringstream oss;
            oss << retStatus.toString() << "\n";
            printMongoOplogHelp(options, &oss);
            return Status(ErrorCodes::FailedToParse, oss.str());
        }

        return Status::OK();
    }
} // namespace mongo

class OplogTool : public Tool {
public:
    OplogTool() : Tool( "oplog" ) { }

    virtual void printHelp( ostream & out ) {
        printMongoOplogHelp(options, &out);
    }

    int run() {

        if ( ! hasParam( "from" ) ) {
            log() << "need to specify --from" << endl;
            return -1;
        }

        Client::initThread( "oplogreplay" );

        log() << "going to connect" << endl;
        
        OplogReader r;
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

REGISTER_MONGO_TOOL(OplogTool);
