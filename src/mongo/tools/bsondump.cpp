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

#include <fcntl.h>

#include "mongo/base/init.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/tools/tool.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/mmap.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/text.h"

using namespace mongo;

namespace mongo {
    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                              MONGO_NO_PREREQUISITES,
                              ("default"))(InitializerContext* context) {

        options = moe::OptionSection( "options" );
        moe::OptionsParser parser;

        Status retStatus = addBSONDumpOptions(&options);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = parser.run(options, context->args(), context->env(), &_params);
        if (!retStatus.isOK()) {
            std::ostringstream oss;
            oss << retStatus.toString() << "\n";
            printBSONDumpHelp(options, &oss);
            return Status(ErrorCodes::FailedToParse, oss.str());
        }

        return Status::OK();
    }
} // namespace mongo

class BSONDump : public BSONTool {

    enum OutputType { JSON , DEBUG } _type;

public:

    BSONDump() : BSONTool( "bsondump" ) {
        _noconnection = true;
    }

    virtual void printHelp(ostream& out) {
        printBSONDumpHelp(options, &out);
    }

    virtual int doRun() {
        {
            string t = getParam( "type" );
            if ( t == "json" )
                _type = JSON;
            else if ( t == "debug" )
                _type = DEBUG;
            else {
                cerr << "bad type: " << t << endl;
                return 1;
            }
        }

        boost::filesystem::path root = getParam( "file" );
        if ( root == "" ) {
            std::cout << "Display BSON objects in a data file.\n" << std::endl;
            std::cout << "usage: " << _name << " [options] <bson filename>" << std::endl;
            return 1;
        }

        processFile( root );
        return 0;
    }

    bool debug( const BSONObj& o , int depth=0) {
        string prefix = "";
        for ( int i=0; i<depth; i++ ) {
            prefix += "\t\t\t";
        }

        int read = 4;

        try {
            cout << prefix << "--- new object ---\n";
            cout << prefix << "\t size : " << o.objsize() << "\n";
            BSONObjIterator i(o);
            while ( i.more() ) {
                BSONElement e = i.next();
                cout << prefix << "\t\t " << e.fieldName() << "\n" << prefix << "\t\t\t type:" << setw(3) << e.type() << " size: " << e.size() << endl;
                if ( ( read + e.size() ) > o.objsize() ) {
                    cout << prefix << " SIZE DOES NOT WORK" << endl;
                    return false;
                }
                read += e.size();
                try {
                    e.validate();
                    if ( e.isABSONObj() ) {
                        if ( ! debug( e.Obj() , depth + 1 ) ) {
                            //return false;
                            cout << prefix << "\t\t\t BAD BAD BAD" << endl;
                            
                            if ( e.size() < 1000 ) {
                                cout << "---\n" << e.Obj().hexDump() << "\n---" << endl;
                            }
                        }
                    }
                    else if ( e.type() == String && ! isValidUTF8( e.valuestr() ) ) {
                        cout << prefix << "\t\t\t" << "bad utf8 String!" << endl;
                    }
                    else if ( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1)) ) {
                        cout << prefix << "\t\t\t" << e << endl;
                    }

                }
                catch ( std::exception& e ) {
                    cout << prefix << "\t\t\t bad value: " << e.what() << endl;
                }
            }
        }
        catch ( std::exception& e ) {
            cout << prefix << "\tbad\t" << e.what() << endl;
            cout << "----\n" << o.hexDump() << "\n---" << endl;
        }
        return true;
    }

    virtual void gotObject( const BSONObj& o ) {
        switch ( _type ) {
        case JSON:
            cout << o.jsonString( TenGen ) << endl;
            break;
        case DEBUG:
            debug(o);
            break;
        default:
            cerr << "bad type? : " << _type << endl;
        }
    }
};

REGISTER_MONGO_TOOL(BSONDump);
