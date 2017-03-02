/**
 *    Copyright (C) 2008-2015 BongoDB Inc.
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

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kDefault

#include "bongo/platform/basic.h"

#include "bongo/dbtests/framework.h"

#include <string>

#include "bongo/base/checked_cast.h"
#include "bongo/base/status.h"
#include "bongo/db/client.h"
#include "bongo/db/concurrency/lock_state.h"
#include "bongo/db/dbdirectclient.h"
#include "bongo/db/op_observer_noop.h"
#include "bongo/db/s/sharding_state.h"
#include "bongo/db/service_context.h"
#include "bongo/db/service_context_d.h"
#include "bongo/dbtests/dbtests.h"
#include "bongo/dbtests/framework_options.h"
#include "bongo/scripting/dbdirectclient_factory.h"
#include "bongo/scripting/engine.h"
#include "bongo/stdx/mutex.h"
#include "bongo/util/assert_util.h"
#include "bongo/util/exit.h"
#include "bongo/util/log.h"
#include "bongo/util/scopeguard.h"
#include "bongo/util/version.h"

namespace bongo {
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

    checked_cast<ServiceContextBongoD*>(globalServiceContext)->createLockFile();
    globalServiceContext->initializeGlobalStorageEngine();
    globalServiceContext->setOpObserver(stdx::make_unique<OpObserverNoop>());

    int ret = unittest::Suite::run(frameworkGlobalParams.suites,
                                   frameworkGlobalParams.filter,
                                   frameworkGlobalParams.runsPerTest);

    // So everything shuts down cleanly
    exitCleanly((ExitCode)ret);
    return ret;
}

}  // namespace dbtests

}  // namespace bongo
