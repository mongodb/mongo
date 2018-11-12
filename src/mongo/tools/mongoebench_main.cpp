
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>

#include "mongo/base/init.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/service_context.h"
#include "mongo/embedded/embedded.h"
#include "mongo/embedded/embedded_options.h"
#include "mongo/embedded/embedded_options_helpers.h"
#include "mongo/scripting/bson_template_evaluator.h"
#include "mongo/shell/bench.h"
#include "mongo/tools/mongoebench_options.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {

/**
 * DBDirectClientWithOwnOpCtx is a version of DBDirectClient that owns its own OperationContext.
 *
 * Since benchRun originally existed only in the mongo shell and was used with only
 * DBClientConnections, there isn't a part of the BenchRunConfig or BenchRunWorker interfaces that
 * is aware of having an OperationContext. In particular, the mongo shell lacks a ServiceContext and
 * therefore also lacks the rest of the Client and OperationContext hierarchy. We shove an
 * OperationContext onto the DBDirectClient to work around this limitation for mongoebench to work.
 */
class DBDirectClientWithOwnOpCtx : public DBDirectClient {
public:
    DBDirectClientWithOwnOpCtx()
        : DBDirectClient(nullptr), _threadClient(getGlobalServiceContext()) {
        _opCtx = cc().makeOperationContext();
        setOpCtx(_opCtx.get());
    }

private:
    ThreadClient _threadClient;
    ServiceContext::UniqueOperationContext _opCtx;
};

MONGO_INITIALIZER_WITH_PREREQUISITES(SignalProcessingStartup, ("ThreadNameInitializer"))
(InitializerContext*) {
    // Make sure we call this as soon as possible but before any other threads are started. Before
    // embedded::initialize is too early and after is too late. So instead we hook in during the
    // global initialization at the right place.
    startSignalProcessingThread();
    return Status::OK();
}

int mongoeBenchMain(int argc, char* argv[], char** envp) {
    ServiceContext* serviceContext = nullptr;

    registerShutdownTask([&]() {
        if (serviceContext) {
            embedded::shutdown(serviceContext);
        }
    });

    setupSignalHandlers();

    log() << "MongoDB embedded benchRun application, for testing purposes only";

    try {
        optionenvironment::OptionSection startupOptions("Options");
        uassertStatusOK(embedded::addOptions(&startupOptions));
        uassertStatusOK(addMongoeBenchOptions(&startupOptions));
        uassertStatusOK(
            embedded_integration_helpers::parseCommandLineOptions(argc, argv, startupOptions));
        serviceContext = embedded::initialize(nullptr);
    } catch (const std::exception& ex) {
        error() << ex.what();
        return EXIT_BADOPTIONS;
    }

    // If a "pre" section was present in the benchRun config file, then we run its operations once
    // before running the operations from the "ops" section.
    if (mongoeBenchGlobalParams.preConfig) {
        auto conn = mongoeBenchGlobalParams.preConfig->createConnection();
        boost::optional<LogicalSessionIdToClient> lsid;

        PseudoRandom rng(mongoeBenchGlobalParams.preConfig->randomSeed);
        BsonTemplateEvaluator bsonTemplateEvaluator(mongoeBenchGlobalParams.preConfig->randomSeed);
        BenchRunStats stats;
        BenchRunOp::State state(&rng, &bsonTemplateEvaluator, &stats);

        for (auto&& op : mongoeBenchGlobalParams.preConfig->ops) {
            op.executeOnce(conn.get(), lsid, *mongoeBenchGlobalParams.preConfig, &state);
        }
    }

    // If an "ops" section was present in the benchRun config file, then we repeatedly run its
    // operations across the configured number of threads for the configured number of seconds.
    if (mongoeBenchGlobalParams.opsConfig) {
        const double seconds = mongoeBenchGlobalParams.opsConfig->seconds;
        auto runner = std::make_unique<BenchRunner>(mongoeBenchGlobalParams.opsConfig.release());
        runner->start();

        sleepmillis(static_cast<long long>(seconds * 1000));

        BSONObj stats = BenchRunner::finish(runner.release());
        log() << "writing stats to " << mongoeBenchGlobalParams.outputFile.string() << ": "
              << stats;

        boost::filesystem::ofstream outfile(mongoeBenchGlobalParams.outputFile);
        outfile << stats.jsonString() << '\n';
    }

    shutdown(EXIT_CLEAN);
}

}  // namespace

MONGO_REGISTER_SHIM(BenchRunConfig::createConnectionImpl)
(const BenchRunConfig& config)->std::unique_ptr<DBClientBase> {
    return std::unique_ptr<DBClientBase>(new DBDirectClientWithOwnOpCtx());
}

}  // namespace mongo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters as
// main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters. The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent and
// makes them available through the argv() and envp() members. This enables mongoeBenchMain() to
// process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    mongo::WindowsCommandLine wcl(argc, argvW, envpW);
    return mongo::mongoeBenchMain(argc, wcl.argv(), wcl.envp());
}
#else
int main(int argc, char* argv[], char** envp) {
    return mongo::mongoeBenchMain(argc, argv, envp);
}
#endif
