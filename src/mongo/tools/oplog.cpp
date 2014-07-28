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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include <fstream>
#include <iostream>

#include "mongo/db/json.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/tools/mongooplog_options.h"
#include "mongo/tools/tool.h"
#include "mongo/util/options_parser/option_section.h"

using namespace mongo;

class OplogTool : public Tool {
public:
    OplogTool() : Tool() { }

    virtual void printHelp( ostream & out ) {
        printMongoOplogHelp(&out);
    }

    int run() {

        Client::initThread( "oplogreplay" );

        toolInfoLog() << "going to connect" << std::endl;
        
        repl::OplogReader r;
        r.setTailingQueryOptions( QueryOption_SlaveOk | QueryOption_AwaitData );

        bool connected = r.connect(mongoOplogGlobalParams.from);

        if (!connected)
        {
            toolInfoLog() << "unable to connect to " << mongoOplogGlobalParams.from << std::endl;
            return -1;
        }

        toolInfoLog() << "connected" << std::endl;

        OpTime start(time(0) - mongoOplogGlobalParams.seconds, 0);
        toolInfoLog() << "starting from " << start.toStringPretty() << std::endl;

        r.tailingQueryGTE(mongoOplogGlobalParams.ns.c_str(), start);

        int num = 0;
        while ( r.more() ) {
            BSONObj o = r.next();
            if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(2))) {
                toolInfoLog() << o << std::endl;
            }
            
            if ( o["$err"].type() ) {
                toolError() << "error getting oplog" << std::endl;
                toolError() << o << std::endl;
                return -1;
            }
                

            bool print = ++num % 100000 == 0;
            if (print) {
                toolInfoLog() << num << "\t" << o << std::endl;
            }
            
            if ( o["op"].String() == "n" )
                continue;

            BSONObjBuilder b( o.objsize() + 32 );
            BSONArrayBuilder updates( b.subarrayStart( "applyOps" ) );
            updates.append( o );
            updates.done();

            BSONObj c = b.obj();
            
            BSONObj res;
            bool ok = conn().runCommand( "admin" , c , res );
            if (!ok) {
                toolError() << res << std::endl;
            } else if (print) {
                toolInfoLog() << res << std::endl;
            }
        }

        return 0;
    }
};

REGISTER_MONGO_TOOL(OplogTool);
