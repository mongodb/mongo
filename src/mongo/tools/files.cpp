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
#include <pcrecpp.h>

#include "mongo/base/init.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/gridfs.h"
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

        Status retStatus = addMongoFilesOptions(&options);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = parser.run(options, context->args(), context->env(), &_params);
        if (!retStatus.isOK()) {
            std::ostringstream oss;
            oss << retStatus.toString() << "\n";
            printMongoFilesHelp(options, &oss);
            return Status(ErrorCodes::FailedToParse, oss.str());
        }

        return Status::OK();
    }
} // namespace mongo

class Files : public Tool {
public:
    Files() : Tool( "files" ) { }

    virtual void printHelp( ostream & out ) {
        printMongoFilesHelp(options, &out);
    }

    void display( GridFS * grid , BSONObj obj ) {
        auto_ptr<DBClientCursor> c = grid->list( obj );
        while ( c->more() ) {
            BSONObj obj = c->next();
            cout
                    << obj["filename"].str() << "\t"
                    << (long)obj["length"].number()
                    << endl;
        }
    }

    int run() {
        string cmd = getParam( "command" );
        if ( cmd.size() == 0 ) {
            cerr << "ERROR: need command" << endl << endl;
            printHelp(cout);
            return -1;
        }

        GridFS g( conn() , _db );

        string filename = getParam( "file" );

        if ( cmd == "list" ) {
            BSONObjBuilder b;
            if ( filename.size() ) {
                b.appendRegex( "filename" , (string)"^" +
                               pcrecpp::RE::QuoteMeta( filename ) );
            }
            
            display( &g , b.obj() );
            return 0;
        }

        if ( filename.size() == 0 ) {
            cerr << "ERROR: need a filename" << endl << endl;
            printHelp(cout);
            return -1;
        }

        if ( cmd == "search" ) {
            BSONObjBuilder b;
            b.appendRegex( "filename" , filename );
            display( &g , b.obj() );
            return 0;
        }

        if ( cmd == "get" ) {
            GridFile f = g.findFile( filename );
            if ( ! f.exists() ) {
                cerr << "ERROR: file not found" << endl;
                return -2;
            }

            string out = getParam("local", f.getFilename());
            f.write( out );

            if (out != "-")
                cout << "done write to: " << out << endl;

            return 0;
        }

        if ( cmd == "put" ) {
            const string& infile = getParam("local", filename);
            const string& type = getParam("type", "");

            BSONObj file = g.storeFile(infile, filename, type);
            cout << "added file: " << file << endl;

            if (hasParam("replace")) {
                auto_ptr<DBClientCursor> cursor = conn().query(_db+".fs.files", BSON("filename" << filename << "_id" << NE << file["_id"] ));
                while (cursor->more()) {
                    BSONObj o = cursor->nextSafe();
                    conn().remove(_db+".fs.files", BSON("_id" << o["_id"]));
                    conn().remove(_db+".fs.chunks", BSON("_id" << o["_id"]));
                    cout << "removed file: " << o << endl;
                }

            }

            conn().getLastError();
            cout << "done!" << endl;
            return 0;
        }

        if ( cmd == "delete" ) {
            g.removeFile(filename);
            conn().getLastError();
            cout << "done!" << endl;
            return 0;
        }

        cerr << "ERROR: unknown command '" << cmd << "'" << endl << endl;
        printHelp(cout);
        return -1;
    }
};

REGISTER_MONGO_TOOL(Files);
