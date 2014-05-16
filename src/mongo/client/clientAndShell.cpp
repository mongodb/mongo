// clientAndShell.cpp

/*    Copyright 2009 10gen Inc.
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

#include "mongo/client/clientOnly-private.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/server_options.h"
#include "mongo/s/shard.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/timer.h"

namespace mongo {

    const char * curNs = "in client mode";

    bool dbexitCalled = false;
    // This mutex helps the shell serialize output on exit,
    // to avoid deadlocks at shutdown.  So it also protects
    // the global dbexitCalled.
    namespace shell_utils {
        mongo::mutex &mongoProgramOutputMutex(*(new mongo::mutex("mongoProgramOutputMutex")));
    }

    void dbexit( ExitCode returnCode, const char *whyMsg ) {
        {
            mongo::mutex::scoped_lock lk( shell_utils::mongoProgramOutputMutex );
            dbexitCalled = true;
        }
        out() << "dbexit called" << endl;
        if ( whyMsg )
            out() << " b/c " << whyMsg << endl;
        out() << "exiting" << endl;
        ::_exit( returnCode );
    }

    bool inShutdown() {
        return dbexitCalled;
    }

    bool haveLocalShardingInfo( const string& ns ) {
        return false;
    }

    DBClientBase * createDirectClient() {
        uassert( 10256 ,  "no createDirectClient in clientOnly" , 0 );
        return 0;
    }

    void Shard::getAllShards( vector<Shard>& all ) {
        verify(0);
    }

    bool Shard::isAShardNode( const string& ident ) {
        verify(0);
        return false;
    }

    ClientBasic* ClientBasic::getCurrent() {
        return 0;
    }

    bool ClientBasic::hasCurrent() {
        return false;
    }
}
