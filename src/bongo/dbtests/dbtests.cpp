// #file dbtests.cpp : Runs db unit tests.
//

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

#include "bongo/platform/basic.h"

#include "bongo/dbtests/dbtests.h"

#include "bongo/base/init.h"
#include "bongo/base/initializer.h"
#include "bongo/client/dbclientinterface.h"
#include "bongo/db/auth/authorization_manager.h"
#include "bongo/db/auth/authorization_manager_global.h"
#include "bongo/db/catalog/index_create.h"
#include "bongo/db/commands.h"
#include "bongo/db/db_raii.h"
#include "bongo/db/index/index_descriptor.h"
#include "bongo/db/logical_clock.h"
#include "bongo/db/repl/replication_coordinator_global.h"
#include "bongo/db/repl/replication_coordinator_mock.h"
#include "bongo/db/server_options.h"
#include "bongo/db/service_context.h"
#include "bongo/db/service_context_d.h"
#include "bongo/db/time_proof_service.h"
#include "bongo/db/wire_version.h"
#include "bongo/dbtests/framework.h"
#include "bongo/scripting/engine.h"
#include "bongo/stdx/memory.h"
#include "bongo/util/quick_exit.h"
#include "bongo/util/signal_handlers_synchronous.h"
#include "bongo/util/startup_test.h"
#include "bongo/util/text.h"

namespace bongo {
namespace dbtests {
namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

BONGO_INITIALIZER(FeatureCompatibilityVersionForTest)(InitializerContext* context) {
    serverGlobalParams.featureCompatibility.version.store(
        ServerGlobalParams::FeatureCompatibility::Version::k34);
    return Status::OK();
}
}  // namespace

void initWireSpec() {
    WireSpec& spec = WireSpec::instance();
    // accept from any version
    spec.incoming.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.incoming.maxWireVersion = COMMANDS_ACCEPT_WRITE_CONCERN;
    // connect to any version
    spec.outgoing.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.outgoing.maxWireVersion = COMMANDS_ACCEPT_WRITE_CONCERN;
}

Status createIndex(OperationContext* txn, StringData ns, const BSONObj& keys, bool unique) {
    BSONObjBuilder specBuilder;
    specBuilder.append("name", DBClientBase::genIndexName(keys));
    specBuilder.append("ns", ns);
    specBuilder.append("key", keys);
    specBuilder.append("v", static_cast<int>(kIndexVersion));
    if (unique) {
        specBuilder.appendBool("unique", true);
    }
    return createIndexFromSpec(txn, ns, specBuilder.done());
}

Status createIndexFromSpec(OperationContext* txn, StringData ns, const BSONObj& spec) {
    AutoGetOrCreateDb autoDb(txn, nsToDatabaseSubstring(ns), MODE_X);
    Collection* coll;
    {
        WriteUnitOfWork wunit(txn);
        coll = autoDb.getDb()->getOrCreateCollection(txn, ns);
        invariant(coll);
        wunit.commit();
    }
    MultiIndexBlock indexer(txn, coll);
    Status status = indexer.init(spec).getStatus();
    if (status == ErrorCodes::IndexAlreadyExists) {
        return Status::OK();
    }
    if (!status.isOK()) {
        return status;
    }
    status = indexer.insertAllDocumentsInCollection();
    if (!status.isOK()) {
        return status;
    }
    WriteUnitOfWork wunit(txn);
    indexer.commit();
    wunit.commit();
    return Status::OK();
}

}  // namespace dbtests
}  // namespace bongo


int dbtestsMain(int argc, char** argv, char** envp) {
    Command::testCommandsEnabled = true;
    ::bongo::setupSynchronousSignalHandlers();
    bongo::dbtests::initWireSpec();
    bongo::runGlobalInitializersOrDie(argc, argv, envp);
    repl::ReplSettings replSettings;
    replSettings.setOplogSizeBytes(10 * 1024 * 1024);
    ServiceContext* service = getGlobalServiceContext();

    std::array<std::uint8_t, 20> tempKey = {};
    TimeProofService::Key key(std::move(tempKey));
    auto timeProofService = stdx::make_unique<TimeProofService>(std::move(key));
    auto logicalClock =
        stdx::make_unique<LogicalClock>(service, std::move(timeProofService), false);
    LogicalClock::set(service, std::move(logicalClock));

    repl::setGlobalReplicationCoordinator(
        new repl::ReplicationCoordinatorMock(service, replSettings));
    repl::getGlobalReplicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY);
    getGlobalAuthorizationManager()->setAuthEnabled(false);
    ScriptEngine::setup();
    StartupTest::runTests();
    return bongo::dbtests::runDbTests(argc, argv);
}

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables dbtestsMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = dbtestsMain(argc, wcl.argv(), wcl.envp());
    quickExit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = dbtestsMain(argc, argv, envp);
    quickExit(exitCode);
}
#endif
