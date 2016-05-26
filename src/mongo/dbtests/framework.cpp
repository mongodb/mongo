/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#include "mongo/dbtests/framework.h"

#include <string>

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/dbtests/framework_options.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/version.h"

namespace mongo {
namespace dbtests {

int runDbTests(int argc, char** argv) {
    frameworkGlobalParams.perfHist = 1;
    frameworkGlobalParams.seed = time(0);
    frameworkGlobalParams.runsPerTest = 1;

    registerShutdownTask([] {
        // We drop the scope cache because leak sanitizer can't see across the
        // thread we use for proxying MozJS requests. Dropping the cache cleans up
        // the memory and makes leak sanitizer happy.
        ScriptEngine::dropScopeCache();

        // We may be shut down before we have a global storage
        // engine.
        if (!getGlobalServiceContext()->getGlobalStorageEngine())
            return;

        getGlobalServiceContext()->shutdownGlobalStorageEngineCleanly();
    });

    Client::initThread("testsuite");

    auto globalServiceContext = getGlobalServiceContext();

    // DBTests run as if in the database, so allow them to create direct clients.
    DBDirectClientFactory::get(globalServiceContext)
        .registerImplementation([](OperationContext* txn) {
            return std::unique_ptr<DBClientBase>(new DBDirectClient(txn));
        });

    srand((unsigned)frameworkGlobalParams.seed);
    printBuildInfo();

    checked_cast<ServiceContextMongoD*>(globalServiceContext)->createLockFile();
    globalServiceContext->initializeGlobalStorageEngine();

    int ret = unittest::Suite::run(frameworkGlobalParams.suites,
                                   frameworkGlobalParams.filter,
                                   frameworkGlobalParams.runsPerTest);

    // So everything shuts down cleanly
    exitCleanly((ExitCode)ret);
    return ret;
}

}  // namespace dbtests

}  // namespace mongo
