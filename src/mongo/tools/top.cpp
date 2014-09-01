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

#include "mongo/platform/basic.h"

#include <fstream>
#include <iomanip>
#include <iostream>

#include "mongo/db/json.h"
#include "mongo/tools/stat_util.h"
#include "mongo/tools/mongotop_options.h"
#include "mongo/tools/tool.h"
#include "mongo/util/options_parser/option_section.h"

namespace mongo {

    class TopTool : public Tool {
    public:

        TopTool() : Tool() {
            _autoreconnect = true;
        }

        virtual void printHelp( ostream & out ) {
            printMongoTopHelp(&out);
        }

        NamespaceStats getData() {
            if (mongoTopGlobalParams.useLocks)
                return getDataLocks();
            return getDataTop();
        }

        NamespaceStats getDataLocks() {

            BSONObj out;
            if (!conn().simpleCommand(toolGlobalParams.db, &out, "serverStatus")) {
                toolError() << "error: " << out << std::endl;
                return NamespaceStats();
            }

            return StatUtil::parseServerStatusLocks( out.getOwned() );
        }

        NamespaceStats getDataTop() {
            NamespaceStats stats;

            BSONObj out;
            if (!conn().simpleCommand(toolGlobalParams.db, &out, "top")) {
                toolError() << "error: " << out << std::endl;
                return stats;
            }

            if ( ! out["totals"].isABSONObj() ) {
                toolError() << "error: invalid top\n" << out << std::endl;
                return stats;
            }

            out = out["totals"].Obj().getOwned();

            BSONObjIterator i( out );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( ! e.isABSONObj() )
                    continue;

                NamespaceInfo& s = stats[e.fieldName()];
                s.ns = e.fieldName();
                s.read = e.Obj()["readLock"].Obj()["time"].numberLong() / 1000;
                s.write = e.Obj()["writeLock"].Obj()["time"].numberLong() / 1000;
            }

            return stats;
        }
        
        void printDiff( const NamespaceStats& prev , const NamespaceStats& now ) {
            if ( prev.size() == 0 || now.size() == 0 ) {
                cout << "." << endl;
                return;
            }
            
            vector<NamespaceDiff> data = StatUtil::computeDiff( prev , now );
            
            unsigned longest = 30;
            
            for ( unsigned i=0; i < data.size(); i++ ) {
                const string& ns = data[i].ns;

                if (!mongoTopGlobalParams.useLocks && ns.find('.') == string::npos)
                    continue;

                if ( ns.size() > longest )
                    longest = ns.size();
            }
            
            int numberWidth = 10;

            cout << "\n"
                 << setw(longest) << (mongoTopGlobalParams.useLocks ? "db" : "ns")
                 << setw(numberWidth+2) << "total"
                 << setw(numberWidth+2) << "read"
                 << setw(numberWidth+2) << "write"
                 << "\t\t" << terseCurrentTime()
                 << endl;
            for ( int i=data.size()-1; i>=0 && data.size() - i < 10 ; i-- ) {
                
                if (!mongoTopGlobalParams.useLocks && data[i].ns.find('.') == string::npos)
                    continue;

                cout << setw(longest) << data[i].ns 
                     << setw(numberWidth) << setprecision(3) << data[i].total() << "ms"
                     << setw(numberWidth) << setprecision(3) << data[i].read << "ms"
                     << setw(numberWidth) << setprecision(3) << data[i].write << "ms"
                     << endl;
            }

        }

        int run() {
            if (isMongos()) {
                toolError() << "mongotop only works on instances of mongod." << std::endl;
                return EXIT_FAILURE;
            }

            NamespaceStats prev = getData();

            while ( true ) {
                sleepsecs(mongoTopGlobalParams.sleep);
                
                NamespaceStats now;
                try {
                    now = getData();
                }
                catch ( std::exception& e ) {
                    toolError() << "can't get data: " << e.what() << std::endl;
                    continue;
                }

                if ( now.size() == 0 )
                    return -2;
                
                try {
                    printDiff( prev , now );
                }
                catch ( AssertionException& e ) {
                    toolError() << "\nerror: " << e.what() << std::endl;
                }

                prev = now;
            }

            return 0;
        }
    };

    REGISTER_MONGO_TOOL(TopTool);

}
