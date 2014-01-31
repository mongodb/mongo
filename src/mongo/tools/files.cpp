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
#include <pcrecpp.h>

#include "mongo/client/dbclientcursor.h"
#include "mongo/client/gridfs.h"
#include "mongo/tools/mongofiles_options.h"
#include "mongo/tools/tool.h"
#include "mongo/util/options_parser/option_section.h"

using namespace mongo;

class Files : public Tool {
public:
    Files() : Tool() { }

    virtual void printHelp( ostream & out ) {
        printMongoFilesHelp(&out);
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
        if (mongoFilesGlobalParams.command.size() == 0) {
            cerr << "ERROR: need command" << endl << endl;
            printHelp(cout);
            return -1;
        }

        GridFS g(conn(), toolGlobalParams.db);

        if (mongoFilesGlobalParams.command == "list") {
            BSONObjBuilder b;
            if (mongoFilesGlobalParams.gridFSFilename.size()) {
                b.appendRegex( "filename" , (string)"^" +
                               pcrecpp::RE::QuoteMeta(mongoFilesGlobalParams.gridFSFilename) );
            }
            
            display( &g , b.obj() );
            return 0;
        }

        if (mongoFilesGlobalParams.gridFSFilename.size() == 0) {
            cerr << "ERROR: need a filename" << endl << endl;
            printHelp(cout);
            return -1;
        }

        if (mongoFilesGlobalParams.command == "search") {
            BSONObjBuilder b;
            b.appendRegex("filename", mongoFilesGlobalParams.gridFSFilename);
            display( &g , b.obj() );
            return 0;
        }

        if (mongoFilesGlobalParams.command == "get") {
            GridFile f = g.findFile(mongoFilesGlobalParams.gridFSFilename);
            if ( ! f.exists() ) {
                cerr << "ERROR: file not found" << endl;
                return -2;
            }

            f.write(mongoFilesGlobalParams.localFile);

            if (mongoFilesGlobalParams.localFile != "-") {
                toolInfoOutput() << "done write to: " << mongoFilesGlobalParams.localFile
                                 << std::endl;
            }

            return 0;
        }

        if (mongoFilesGlobalParams.command == "put") {
            BSONObj file = g.storeFile(mongoFilesGlobalParams.localFile,
                                       mongoFilesGlobalParams.gridFSFilename,
                                       mongoFilesGlobalParams.contentType);
            toolInfoOutput() << "added file: " << file << std::endl;

            if (mongoFilesGlobalParams.replace) {
                auto_ptr<DBClientCursor> cursor =
                    conn().query(toolGlobalParams.db + ".fs.files",
                                 BSON("filename" << mongoFilesGlobalParams.gridFSFilename
                                                 << "_id" << NE << file["_id"] ));
                while (cursor->more()) {
                    BSONObj o = cursor->nextSafe();
                    conn().remove(toolGlobalParams.db + ".fs.files", BSON("_id" << o["_id"]));
                    conn().remove(toolGlobalParams.db + ".fs.chunks", BSON("_id" << o["_id"]));
                    toolInfoOutput() << "removed file: " << o << std::endl;
                }

            }

            conn().getLastError();
            toolInfoOutput() << "done!" << std::endl;
            return 0;
        }

        if (mongoFilesGlobalParams.command == "delete") {
            g.removeFile(mongoFilesGlobalParams.gridFSFilename);
            conn().getLastError();
            toolInfoOutput() << "done!" << std::endl;
            return 0;
        }

        cerr << "ERROR: unknown command '" << mongoFilesGlobalParams.command << "'" << endl << endl;
        printHelp(cout);
        return -1;
    }
};

REGISTER_MONGO_TOOL(Files);
