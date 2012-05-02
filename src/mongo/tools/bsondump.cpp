// restore.cpp

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
#include "mongo/client/dbclientcursor.h"
#include "../util/mmap.h"
#include "../util/text.h"
#include "tool.h"

#include <boost/program_options.hpp>

#include <fcntl.h>

using namespace mongo;

namespace po = boost::program_options;

class BSONDump : public BSONTool {

    enum OutputType { JSON , DEBUG } _type;

public:

    BSONDump() : BSONTool( "bsondump", NONE ) {
        add_options()
        ("type" , po::value<string>()->default_value("json") , "type of output: json,debug" )
        ;
        add_hidden_options()
        ("file" , po::value<string>() , ".bson file" )
        ;
        addPositionArg( "file" , 1 );
        _noconnection = true;
    }

    virtual void printExtraHelp(ostream& out) {
        out << "Display BSON objects in a data file.\n" << endl;
        out << "usage: " << _name << " [options] <bson filename>" << endl;
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
            printExtraHelp(cout);
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
                    else if ( logLevel > 0 ) {
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

int main( int argc , char ** argv ) {
    BSONDump dump;
    return dump.main( argc , argv );
}
