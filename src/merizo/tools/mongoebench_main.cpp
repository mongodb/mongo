/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kDefault

#include "merizo/platform/basic.h"

#include <boost/filesystem.hpp>

#include "merizo/base/init.h"
#include "merizo/db/dbdirectclient.h"
#include "merizo/db/service_context.h"
#include "merizo/embedded/embedded.h"
#include "merizo/embedded/embedded_options.h"
#include "merizo/embedded/embedded_options_helpers.h"
#include "merizo/scripting/bson_template_evaluator.h"
#include "merizo/shell/bench.h"
#include "merizo/tools/merizoebench_options.h"
#include "merizo/tools/merizoebench_options_gen.h"
#include "merizo/util/exit.h"
#include "merizo/util/log.h"
#include "merizo/util/options_parser/option_section.h"
#include "merizo/util/signal_handlers.h"
#include "merizo/util/text.h"

namespace merizo {
namespace {

/**
 * DBDirectClientWithOwnOpCtx is a version of DBDirectClient that owns its own OperationContext.
 *
 * Since benchRun originally existed only in the merizo shell and was used with only
 * DBClientConnections, there isn't a part of the BenchRunConfig or BenchRunWorker interfaces that
 * is aware of having an OperationContext. In particular, the merizo shell lacks a ServiceContext and
 * therefore also lacks the rest of the Client and OperationContext hierarchy. We shove an
 * OperationContext onto the DBDirectClient to work around this limitation for merizoebench to work.
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

MERIZO_INITIALIZER_WITH_PREREQUISITES(SignalProcessingStartup, ("ThreadNameInitializer"))
(InitializerContext*) {
    // Make sure we call this as soon as possible but before any other threads are started. Before
    // embedded::initialize is too early and after is too late. So instead we hook in during the
    // global initialization at the right place.
    startSignalProcessingThread();
    return Status::OK();
}

int merizoeBenchMain(int argc, char* argv[], char** envp) {
    ServiceContext* serviceContext = nullptr;

    registerShutdownTask([&]() {
        if (serviceContext) {
            embedded::shutdown(serviceContext);
        }
    });

    setupSignalHandlers();

    log() << "MerizoDB embedded benchRun application, for testing purposes only";

    try {
        optionenvironment::OptionSection startupOptions("Options");
        uassertStatusOK(embedded::addOptions(&startupOptions));
        uassertStatusOK(addMerizoeBenchOptions(&startupOptions));
        uassertStatusOK(
            embedded_integration_helpers::parseCommandLineOptions(argc, argv, startupOptions));
        serviceContext = embedded::initialize(nullptr);
    } catch (const std::exception& ex) {
        error() << ex.what();
        return EXIT_BADOPTIONS;
    }

    // If a "pre" section was present in the benchRun config file, then we run its operations once
    // before running the operations from the "ops" section.
    if (merizoeBenchGlobalParams.preConfig) {
        auto conn = merizoeBenchGlobalParams.preConfig->createConnection();
        boost::optional<LogicalSessionIdToClient> lsid;

        PseudoRandom rng(merizoeBenchGlobalParams.preConfig->randomSeed);
        BsonTemplateEvaluator bsonTemplateEvaluator(merizoeBenchGlobalParams.preConfig->randomSeed);
        BenchRunStats stats;
        BenchRunOp::State state(&rng, &bsonTemplateEvaluator, &stats);

        for (auto&& op : merizoeBenchGlobalParams.preConfig->ops) {
            op.executeOnce(conn.get(), lsid, *merizoeBenchGlobalParams.preConfig, &state);
        }
    }

    // If an "ops" section was present in the benchRun config file, then we repeatedly run its
    // operations across the configured number of threads for the configured number of seconds.
    if (merizoeBenchGlobalParams.opsConfig) {
        const double seconds = merizoeBenchGlobalParams.opsConfig->seconds;
        auto runner = std::make_unique<BenchRunner>(merizoeBenchGlobalParams.opsConfig.release());
        runner->start();

        sleepmillis(static_cast<long long>(seconds * 1000));

        BSONObj stats = BenchRunner::finish(runner.release());
        log() << "writing stats to " << merizoeBenchGlobalParams.outputFile.string() << ": "
              << stats;

        boost::filesystem::ofstream outfile(merizoeBenchGlobalParams.outputFile);
        outfile << stats.jsonString() << '\n';
    }

    shutdown(EXIT_CLEAN);
}

}  // namespace

MERIZO_REGISTER_SHIM(BenchRunConfig::createConnectionImpl)
(const BenchRunConfig& config)->std::unique_ptr<DBClientBase> {
    return std::unique_ptr<DBClientBase>(new DBDirectClientWithOwnOpCtx());
}

}  // namespace merizo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters as
// main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters. The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent and
// makes them available through the argv() and envp() members. This enables merizoeBenchMain() to
// process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    merizo::WindowsCommandLine wcl(argc, argvW, envpW);
    return merizo::merizoeBenchMain(argc, wcl.argv(), wcl.envp());
}
#else
int main(int argc, char* argv[], char** envp) {
    return merizo::merizoeBenchMain(argc, argv, envp);
}
#endif
