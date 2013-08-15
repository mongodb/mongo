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
#include "mongo/tools/stat_util.h"
#include "mongo/tools/tool.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

namespace mongo {
    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                              MONGO_NO_PREREQUISITES,
                              ("default"))(InitializerContext* context) {

        options = moe::OptionSection( "options" );
        moe::OptionsParser parser;

        Status retStatus = addMongoTopOptions(&options);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = parser.run(options, context->args(), context->env(), &_params);
        if (!retStatus.isOK()) {
            std::ostringstream oss;
            oss << retStatus.toString() << "\n";
            printMongoTopHelp(options, &oss);
            return Status(ErrorCodes::FailedToParse, oss.str());
        }

        return Status::OK();
    }
} // namespace mongo

namespace mongo {

    class TopTool : public Tool {
    public:

        TopTool() : Tool( "top" , "admin" ) {
            _sleep = 1;
            _autoreconnect = true;
        }

        virtual void printHelp( ostream & out ) {
            printMongoTopHelp(options, &out);
        }
        
        bool useLocks() {
            return hasParam( "locks" );
        }

        NamespaceStats getData() {
            if ( useLocks() )
                return getDataLocks();
            return getDataTop();
        }

        NamespaceStats getDataLocks() {

            BSONObj out;
            if ( ! conn().simpleCommand( _db , &out , "serverStatus" ) ) {
                cout << "error: " << out << endl;
                return NamespaceStats();
            }

            return StatUtil::parseServerStatusLocks( out.getOwned() );
        }

        NamespaceStats getDataTop() {
            NamespaceStats stats;

            BSONObj out;
            if ( ! conn().simpleCommand( _db , &out , "top" ) ) {
                cout << "error: " << out << endl;
                return stats;
            }

            if ( ! out["totals"].isABSONObj() ) {
                cout << "error: invalid top\n" << out << endl;
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

                if ( ! useLocks() && ns.find( '.' ) == string::npos )
                    continue;

                if ( ns.size() > longest )
                    longest = ns.size();
            }
            
            int numberWidth = 10;

            cout << "\n"
                 << setw(longest) << ( useLocks() ? "db" : "ns" )
                 << setw(numberWidth+2) << "total"
                 << setw(numberWidth+2) << "read"
                 << setw(numberWidth+2) << "write"
                 << "\t\t" << terseCurrentTime()
                 << endl;
            for ( int i=data.size()-1; i>=0 && data.size() - i < 10 ; i-- ) {
                
                if ( ! useLocks() && data[i].ns.find( '.' ) == string::npos )
                    continue;

                cout << setw(longest) << data[i].ns 
                     << setw(numberWidth) << setprecision(3) << data[i].total() << "ms"
                     << setw(numberWidth) << setprecision(3) << data[i].read << "ms"
                     << setw(numberWidth) << setprecision(3) << data[i].write << "ms"
                     << endl;
            }

        }

        int run() {
            _sleep = getParam( "sleep" , _sleep );

            if (isMongos()) {
                log() << "mongotop only works on instances of mongod." << endl;
                return EXIT_FAILURE;
            }

            NamespaceStats prev = getData();

            while ( true ) {
                sleepsecs( _sleep );
                
                NamespaceStats now;
                try {
                    now = getData();
                }
                catch ( std::exception& e ) {
                    cout << "can't get data: " << e.what() << endl;
                    continue;
                }

                if ( now.size() == 0 )
                    return -2;
                
                try {
                    printDiff( prev , now );
                }
                catch ( AssertionException& e ) {
                    cout << "\nerror: " << e.what() << endl;
                }

                prev = now;
            }

            return 0;
        }

    private:
        int _sleep;
    };

    REGISTER_MONGO_TOOL(TopTool);

}
