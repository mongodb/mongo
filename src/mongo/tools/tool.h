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

// Tool.h

#pragma once

#include <string>

#if defined(_WIN32)
#include <io.h>
#endif

#include "mongo/db/instance.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/tools/tool_logger.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/options_parser/environment.h"

using std::string;

namespace mongo {

    class Tool {
    public:
        Tool();
        virtual ~Tool();

        static std::auto_ptr<Tool> (*createInstance)();

        int main( int argc , char ** argv, char ** envp );

        std::string getNS() {
            if (toolGlobalParams.coll.size() == 0) {
                cerr << "no collection specified!" << std::endl;
                throw -1;
            }
            return toolGlobalParams.db + "." + toolGlobalParams.coll;
        }

        std::string getAuthenticationDatabase();

        bool isMaster();
        bool isMongos();

        virtual int run() = 0;

        virtual void printHelp(std::ostream &out) = 0;

    protected:

        mongo::DBClientBase &conn( bool slaveIfPaired = false );

        bool _autoreconnect;

    protected:

        mongo::DBClientBase * _conn;
        mongo::DBClientBase * _slaveConn;

    private:
        void auth();
    };

    class BSONTool : public Tool {
        std::auto_ptr<Matcher> _matcher;

    public:
        BSONTool();

        virtual int doRun() = 0;
        virtual void gotObject( const BSONObj& obj ) = 0;

        virtual int run();

        long long processFile( const boost::filesystem::path& file );

    };

}

#define REGISTER_MONGO_TOOL(TYPENAME) \
    std::auto_ptr<Tool> createInstanceOfThisTool() {return std::auto_ptr<Tool>(new TYPENAME());} \
    std::auto_ptr<Tool> (*Tool::createInstance)() = createInstanceOfThisTool;
