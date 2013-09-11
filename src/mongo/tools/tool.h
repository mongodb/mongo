/*
 *    Copyright (C) 2010 10gen Inc.
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

// Tool.h

#pragma once

#include <string>

#if defined(_WIN32)
#include <io.h>
#endif

#include "mongo/db/instance.h"
#include "mongo/db/matcher.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/options_parser/environment.h"

using std::string;

namespace mongo {

    class Tool {
    public:
        Tool(bool usesstdout=true);
        virtual ~Tool();

        static auto_ptr<Tool> (*createInstance)();

        int main( int argc , char ** argv, char ** envp );

        string getNS() {
            if (toolGlobalParams.coll.size() == 0) {
                cerr << "no collection specified!" << endl;
                throw -1;
            }
            return toolGlobalParams.db + "." + toolGlobalParams.coll;
        }

        string getAuthenticationDatabase();

        void useStandardOutput( bool mode ) {
            _usesstdout = mode;
        }

        bool isMaster();
        bool isMongos();
        
        virtual void preSetup() {}

        virtual int run() = 0;

        virtual void printHelp(ostream &out) = 0;

    protected:

        mongo::DBClientBase &conn( bool slaveIfPaired = false );

        bool _usesstdout;
        bool _autoreconnect;

    protected:

        mongo::DBClientBase * _conn;
        mongo::DBClientBase * _slaveConn;

    private:
        void auth();
    };

    class BSONTool : public Tool {
        auto_ptr<Matcher> _matcher;

    public:
        BSONTool();

        virtual int doRun() = 0;
        virtual void gotObject( const BSONObj& obj ) = 0;

        virtual int run();

        long long processFile( const boost::filesystem::path& file );

    };

}

#define REGISTER_MONGO_TOOL(TYPENAME) \
    auto_ptr<Tool> createInstanceOfThisTool() {return auto_ptr<Tool>(new TYPENAME());} \
    auto_ptr<Tool> (*Tool::createInstance)() = createInstanceOfThisTool;
