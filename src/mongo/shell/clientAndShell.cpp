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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/shell/shell_utils.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"

namespace mongo {

    using std::endl;
    using std::string;
    using std::vector;

    class Client;
    class DBClientBase;
    class OperationContext;


    bool dbexitCalled = false;

    void dbexit( ExitCode returnCode, const char *whyMsg ) {
        {
            stdx::lock_guard<stdx::mutex> lk( shell_utils::mongoProgramOutputMutex );
            dbexitCalled = true;
        }

        log() << "dbexit called" << endl;

        if (whyMsg) {
            log() << " b/c " << whyMsg << endl;
        }

        log() << "exiting" << endl;
        quickExit( returnCode );
    }

    bool inShutdown() {
        return dbexitCalled;
    }

    bool haveLocalShardingInfo( Client* client, const string& ns ) {
        return false;
    }

    DBClientBase* createDirectClient(OperationContext* txn) {
        uassert( 10256 ,  "no createDirectClient in clientOnly" , 0 );
        return 0;
    }

}
