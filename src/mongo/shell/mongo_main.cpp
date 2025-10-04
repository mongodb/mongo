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

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>  // IWYU pragma: keep
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/core/null_deleter.hpp>
#include <boost/exception/exception.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core/core.hpp>
#include <boost/log/core/record_view.hpp>
// IWYU pragma: no_include "boost/log/detail/attachable_sstream_buf.hpp"
// IWYU pragma: no_include "boost/log/detail/locking_ptr.hpp"
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/dbclient_session.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sasl_aws_client_options.h"
#include "mongo/client/sasl_oidc_client_params.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/console.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/linenoise.h"
#include "mongo/shell/mongo_main.h"
#include "mongo/shell/program_runner.h"
#include "mongo/shell/shell_options.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/shell/shell_utils_extended.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/utility.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/allocator_thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/duration.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/file.h"
#include "mongo/util/net/ocsp/ocsp_manager.h"
#include "mongo/util/password.h"
#include "mongo/util/pcre.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"
#include "mongo/util/version/releases.h"

#include <boost/thread/exceptions.hpp>

#ifdef MONGO_CONFIG_GRPC
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#endif

#ifdef _WIN32
#include <io.h>
#include <shlobj.h>

#define SIGKILL 9
#define isatty _isatty
#define fileno _fileno
#else
#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


using namespace std::literals::string_literals;

namespace mongo {
namespace {

bool gotInterrupted = false;
bool inMultiLine = false;
static AtomicWord<bool> atPrompt(false);  // can eval before getting to prompt


// Initialize the featureCompatibilityVersion server parameter since the mongo shell does not have a
// featureCompatibilityVersion document from which to initialize the parameter. The parameter is set
// to the latest version because there is no feature gating that currently occurs at the mongo shell
// level. The server is responsible for rejecting usages of new features if its
// featureCompatibilityVersion is lower.
MONGO_INITIALIZER_WITH_PREREQUISITES(SetFeatureCompatibilityVersionLatest,
                                     ("EndStartupOptionStorage"))
// (Generic FCV reference): This FCV reference should exist across LTS binary versions.
(InitializerContext* context) {
    mongo::serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
}

namespace {
ServiceContext::ConstructorActionRegisterer registerWireSpec{
    "RegisterWireSpec", [](ServiceContext* service) {
        WireSpec::getWireSpec(service).initialize(WireSpec::Specification{});
    }};
}  // namespace

const auto kAuthParam = "authSource"s;

/**
 * Basic Logv2 console backend. Provides scoped logging disable.
 */
class ShellBackend final : public boost::log::sinks::text_ostream_backend {
public:
    void consume(boost::log::record_view const& rec, string_type const& formatted_message) {
        using boost::log::extract;

        auto lk = stdx::lock_guard(mx);
        if (!loggingEnabled &&
            !extract<logv2::LogTag>(logv2::attributes::tags(), rec)
                 .get()
                 .has(logv2::LogTag::kAllowDuringPromptingShell)) {
            return;
        }
        boost::log::sinks::text_ostream_backend::consume(rec, formatted_message);
    }

    struct LoggingDisabledScope {
        LoggingDisabledScope() {
            disableLogging();
        }

        ~LoggingDisabledScope() {
            enableLogging();
        }
    };

private:
    static void enableLogging() {
        auto lk = stdx::lock_guard(mx);
        invariant(!loggingEnabled);
        loggingEnabled = true;
    }

    static void disableLogging() {
        auto lk = stdx::lock_guard(mx);
        invariant(loggingEnabled);
        loggingEnabled = false;
    }

    // This needs to use a mutex rather than an atomic bool because we need to ensure that no more
    // logging will happen once we return from disable().
    static inline stdx::mutex mx;
    static inline bool loggingEnabled = true;
};

/**
 * Formatter to provide specialized formatting for logs from javascript engine
 */
class ShellFormatter final : private logv2::PlainFormatter, private logv2::JSONFormatter {
public:
    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) {
        using namespace logv2;
        using boost::log::extract;

        if (extract<LogTag>(attributes::tags(), rec).get().has(LogTag::kPlainShell)) {
            PlainFormatter::operator()(rec, strm);
        } else {
            JSONFormatter::operator()(rec, strm);
        }
    }
};

enum ShellExitCode : int {
    kDBException = 1,
    kInputFileError = -3,
    kEvalError = -4,
    kMongorcError = -5,
    kUnterminatedProcess = -6,
    kProcessTerminationError = -7,
    kUnexpectedCoreDumpFound = -8,
};

Scope* shellMainScope;

bool isSessionTimedOut() {
    static Date_t previousCommandTime = Date_t::now();
    if (shellGlobalParams.idleSessionTimeout > Seconds(0)) {
        const Date_t now = Date_t::now();

        if (now > (previousCommandTime + shellGlobalParams.idleSessionTimeout)) {
            return true;
        }
        previousCommandTime = now;
    }
    return false;
}

void generateCompletions(const std::string& prefix, std::vector<std::string>& all) {
    if (prefix.find('"') != std::string::npos)
        return;

    try {
        BSONObj args = BSON("0" << prefix);
        shellMainScope->invokeSafe(
            "function callShellAutocomplete(x) {shellAutocomplete(x)}", &args, nullptr);
        BSONObjBuilder b;
        shellMainScope->append(b, "", "__autocomplete__");
        BSONObj res = b.obj();
        BSONObj arr = res.firstElement().Obj();

        BSONObjIterator i(arr);
        while (i.more()) {
            BSONElement e = i.next();
            all.push_back(e.String());
        }
    } catch (...) {
    }
}

void completionHook(const char* text, linenoiseCompletions* lc) {
    std::vector<std::string> all;
    generateCompletions(text, all);

    for (unsigned i = 0; i < all.size(); ++i)
        linenoiseAddCompletion(lc, (char*)all[i].c_str());
}

void shellHistoryInit() {
    Status res = linenoiseHistoryLoad(shell_utils::getHistoryFilePath().string().c_str());
    if (!res.isOK()) {
        std::cout << "Error loading history file: " << res << std::endl;
    }
    linenoiseSetCompletionCallback(completionHook);
}

void shellHistoryDone() {
    Status res = linenoiseHistorySave(shell_utils::getHistoryFilePath().string().c_str());
    if (!res.isOK()) {
        std::cout << "Error saving history file: " << res << std::endl;
    }
    linenoiseHistoryFree();
}
void shellHistoryAdd(const char* line) {
    if (line[0] == '\0')
        return;

    // dont record duplicate lines
    static std::string lastLine;
    if (lastLine == line)
        return;
    lastLine = line;

    // We don't want any .auth() or .createUser() shell helpers added, but we want to
    // be able to add things like `.author`, so be smart about how this is
    // detected by using regular expresions. This is so we can avoid storing passwords
    // in the history file in plaintext.
    static pcre::Regex hiddenHelpers(
        "\\.\\s*(auth|createUser|updateUser|changeUserPassword)\\s*\\(");
    // Also don't want the raw user management commands to show in the shell when run directly
    // via runCommand.
    static pcre::Regex hiddenCommands(
        "(run|admin)Command\\s*\\(\\s*{\\s*(createUser|updateUser)\\s*:");

    static pcre::Regex hiddenFLEConstructor(".*Mongo\\(([\\s\\S]*)secretAccessKey([\\s\\S]*)");
    if (!hiddenHelpers.matchView(line) && !hiddenCommands.matchView(line) &&
        !hiddenFLEConstructor.matchView(line)) {
        linenoiseHistoryAdd(line);
    }
}

void killOps() {
    if (shellGlobalParams.nokillop.load())
        return;

    if (atPrompt.load())
        return;

    sleepmillis(10);  // give current op a chance to finish

    mongo::shell_utils::connectionRegistry.killOperationsOnAllConnections(
        !shellGlobalParams.autoKillOp);
}

extern "C" void quitNicely(int sig) {
    shutdown(ExitCode::clean);
}

// the returned string is allocated with strdup() or malloc() and must be freed by calling free()
char* shellReadline(const char* prompt, int handlesigint = 0) {
    auto lds = ShellBackend::LoggingDisabledScope();
    atPrompt.store(true);

    char* ret = linenoise(prompt);
    if (!ret) {
        gotInterrupted = true;  // got ^C, break out of multiline
    }

    atPrompt.store(false);
    return ret;
}

void setupSignals() {
#ifndef _WIN32
    signal(SIGHUP, quitNicely);
#endif
    signal(SIGINT, quitNicely);
}


std::string finishCode(std::string code) {
    while (!shell_utils::isBalanced(code)) {
        inMultiLine = true;
        code += "\n";
        // cancel multiline if two blank lines are entered
        if (code.find("\n\n\n") != std::string::npos)
            return ";";
        char* line = shellReadline("... ", 1);
        if (gotInterrupted) {
            if (line)
                free(line);
            return "";
        }
        if (!line)
            return "";

        char* linePtr = line;
        while (str::startsWith(linePtr, "... "))
            linePtr += 4;

        code += linePtr;
        free(line);
    }
    return code;
}

bool execPrompt(mongo::Scope& scope, const char* promptFunction, std::string& prompt) {
    std::string execStatement = std::string("__promptWrapper__(") + promptFunction + ");";
    scope.exec("delete __prompt__;", "", false, false, false, 0);
    scope.exec(execStatement, "", false, false, false, 0);
    if (scope.type("__prompt__") == stdx::to_underlying(BSONType::string)) {
        prompt = scope.getString("__prompt__");
        return true;
    }
    return false;
}

/**
 * Edit a variable or input buffer text in an external editor -- EDITOR must be defined
 *
 * @param whatToEdit Name of JavaScript variable to be edited, or any text string
 */
static void edit(const std::string& whatToEdit) {
    // EDITOR may be defined in the JavaScript scope or in the environment
    std::string editor;
    if (shellMainScope->type("EDITOR") == stdx::to_underlying(BSONType::string)) {
        editor = shellMainScope->getString("EDITOR");
    } else {
        static const char* editorFromEnv = getenv("EDITOR");
        if (editorFromEnv) {
            editor = editorFromEnv;
        }
    }
    if (editor.empty()) {
        std::cout << "please define EDITOR as a JavaScript string or as an environment variable"
                  << std::endl;
        return;
    }

    // "whatToEdit" might look like a variable/property name
    bool editingVariable = true;
    for (const char* p = whatToEdit.c_str(); *p; ++p) {
        if (!(ctype::isAlnum(*p) || *p == '_' || *p == '.')) {
            editingVariable = false;
            break;
        }
    }

    std::string js;
    if (editingVariable) {
        // If "whatToEdit" is undeclared or uninitialized, declare
        int varType = shellMainScope->type(whatToEdit.c_str());
        if (varType == stdx::to_underlying(BSONType::undefined)) {
            shellMainScope->exec("var " + whatToEdit, "(shell)", false, true, false);
        }

        // Convert "whatToEdit" to JavaScript (JSON) text
        if (!shellMainScope->exec(
                "__jsout__ = tojson(" + whatToEdit + ")", "tojs", false, false, false))
            return;  // Error already printed

        js = shellMainScope->getString("__jsout__");

        if (strstr(js.c_str(), "[native code]")) {
            std::cout << "can't edit native functions" << std::endl;
            return;
        }
    } else {
        js = whatToEdit;
    }

    // Pick a name to use for the temp file
    std::string filename;
    const int maxAttempts = 10;
    int i;
    for (i = 0; i < maxAttempts; ++i) {
        StringBuilder sb;
#ifdef _WIN32
        char tempFolder[MAX_PATH];
        GetTempPathA(sizeof tempFolder, tempFolder);
        sb << tempFolder << "mongo_edit" << time(0) + i << ".js";
#else
        sb << "/tmp/mongo_edit" << time(nullptr) + i << ".js";
#endif
        filename = sb.str();
        if (!::mongo::shell_utils::fileExists(filename))
            break;
    }
    if (i == maxAttempts) {
        std::cout << "couldn't create unique temp file after " << maxAttempts << " attempts"
                  << std::endl;
        return;
    }

    // Create the temp file
    FILE* tempFileStream;
    tempFileStream = fopen(filename.c_str(), "wt");
    if (!tempFileStream) {
        auto ec = lastPosixError();
        std::cout << "couldn't create temp file (" << filename << "): " << errorMessage(ec)
                  << std::endl;
        return;
    }

    // Write JSON into the temp file
    size_t fileSize = js.size();
    if (fwrite(js.data(), sizeof(char), fileSize, tempFileStream) != fileSize) {
        auto ec = lastPosixError();
        std::cout << "failed to write to temp file: " << errorMessage(ec) << std::endl;
        fclose(tempFileStream);
        remove(filename.c_str());
        return;
    }
    fclose(tempFileStream);

    // Pass file to editor
    StringBuilder sb;
    sb << editor << " " << filename;
    int ret = [&] {
        auto lds = ShellBackend::LoggingDisabledScope();
        return ::system(sb.str().c_str());
    }();
    if (ret) {
        if (ret == -1) {
            auto ec = lastPosixError();
            std::cout << "failed to launch $EDITOR (" << editor << "): " << errorMessage(ec)
                      << std::endl;
        } else
            std::cout << "editor exited with error (" << ret << "), not applying changes"
                      << std::endl;
        remove(filename.c_str());
        return;
    }

    // The editor gave return code zero, so read the file back in
    tempFileStream = fopen(filename.c_str(), "rt");
    if (!tempFileStream) {
        auto ec = lastPosixError();
        std::cout << "couldn't open temp file on return from editor: " << errorMessage(ec)
                  << std::endl;
        remove(filename.c_str());
        return;
    }
    sb.reset();
    int bytes;
    do {
        char buf[1024];
        bytes = fread(buf, sizeof(char), sizeof buf, tempFileStream);
        if (ferror(tempFileStream)) {
            auto ec = lastPosixError();
            std::cout << "failed to read temp file: " << errorMessage(ec) << std::endl;
            fclose(tempFileStream);
            remove(filename.c_str());
            return;
        }
        sb.append(StringData(buf, bytes));
    } while (bytes);

    // Done with temp file, close and delete it
    fclose(tempFileStream);
    remove(filename.c_str());

    if (editingVariable) {
        // Try to execute assignment to copy edited value back into the variable
        const std::string code = whatToEdit + std::string(" = ") + sb.str();
        if (!shellMainScope->exec(code, "tojs", false, true, false)) {
            std::cout << "error executing assignment: " << code << std::endl;
        }
    } else {
        linenoisePreloadBuffer(sb.str().c_str());
    }
}

bool mechanismRequiresPassword(const MongoURI& uri) {
    if (const auto authMechanisms = uri.getOption("authMechanism")) {
        constexpr std::array<StringData, 3> passwordlessMechanisms{
            auth::kMechanismGSSAPI, auth::kMechanismMongoX509, auth::kMechanismMongoOIDC};
        const std::string& authMechanism = authMechanisms.value();
        for (const auto& mechanism : passwordlessMechanisms) {
            if (mechanism == authMechanism) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int mongo_main(int argc, char* argv[]) {
    try {

        registerShutdownTask([] {
            // NOTE: This function may be called at any time. It must not
            // depend on the prior execution of mongo initializers or the
            // existence of threads.
            killOps();
            shellHistoryDone();
        });

        setupSignalHandlers();
        setupSignals();

        // Log to stdout for any early logging before we re-configure the logger
        auto& lv2Manager = logv2::LogManager::global();
        logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
        uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));

        mongo::shell_utils::RecordMyLocation(argv[0]);

        mongo::runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));
        setGlobalServiceContext(ServiceContext::make());

        // TODO This should use a TransportLayerManager or TransportLayerFactory
        auto serviceContext = getGlobalServiceContext();

        startAllocatorThread();

        // Set up the periodic runner for background job execution. This is required to be running
        // before the transport layer is initialized.
        auto runner = makePeriodicRunner(serviceContext);
        serviceContext->setPeriodicRunner(std::move(runner));

        ShardingState::create(serviceContext);

#ifdef MONGO_CONFIG_SSL
        OCSPManager::start(serviceContext);
#endif
        shell_utils::ProgramRegistry::create(serviceContext);

        // hide password from ps output
        redactPasswordOptions(argc, argv);

        ErrorExtraInfo::invariantHaveAllParsers();

        if (!mongo::serverGlobalParams.quiet.load())
            std::cout << mongoShellVersion(VersionInfoInterface::instance()) << std::endl;

        auto consoleSink = boost::make_shared<boost::log::sinks::synchronous_sink<ShellBackend>>();
        consoleSink->set_filter(logv2::ComponentSettingsFilter(lv2Manager.getGlobalDomain(),
                                                               lv2Manager.getGlobalSettings()));
        consoleSink->set_formatter(ShellFormatter());

        consoleSink->locked_backend()->add_stream(
            boost::shared_ptr<std::ostream>(&logv2::Console::out(), boost::null_deleter()));

        consoleSink->locked_backend()->auto_flush();

        // Remove the initial config from above when setting this sink, otherwise we log everything
        // twice.
        lv2Config.makeDisabled();
        uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));

        boost::log::core::get()->add_sink(std::move(consoleSink));

        // Get the URL passed to the shell
        std::string& cmdlineURI = shellGlobalParams.url;

        // Parse the output of getURIFromArgs which will determine if --host passed in a URI
        MongoURI parsedURI;
        parsedURI = uassertStatusOK(MongoURI::parse(
            mongo::shell_utils::getURIFromArgs(cmdlineURI,
                                               str::escape(shellGlobalParams.dbhost),
                                               str::escape(shellGlobalParams.port))));

        // TODO: add in all of the relevant shellGlobalParams to parsedURI
        parsedURI.setOptionIfNecessary("compressors"s, shellGlobalParams.networkMessageCompressors);
        parsedURI.setOptionIfNecessary("authMechanism"s, shellGlobalParams.authenticationMechanism);
        parsedURI.setOptionIfNecessary("authSource"s, shellGlobalParams.authenticationDatabase);
        parsedURI.setOptionIfNecessary("gssapiServiceName"s, shellGlobalParams.gssapiServiceName);
        parsedURI.setOptionIfNecessary("gssapiHostName"s, shellGlobalParams.gssapiHostName);
#ifdef MONGO_CONFIG_GRPC
        parsedURI.setOptionIfNecessary("gRPC"s, shellGlobalParams.gRPC ? "true" : "false");
#endif

        std::vector<std::unique_ptr<transport::TransportLayer>> tls;

        // Create the ASIO transport layer.
        transport::AsioTransportLayer::Options opts;
        opts.enableIPv6 = shellGlobalParams.enableIPv6;
        opts.mode = transport::AsioTransportLayer::Options::kEgress;
        tls.push_back(std::make_unique<transport::AsioTransportLayer>(opts, nullptr));
        auto asioLayer = tls[0].get();

#ifdef MONGO_CONFIG_GRPC
        // The shell will start an egress gRPC layer in addition to the asio one if it was
        // configured to communicate with gRPC. It will decide at runtime during Mongo construction
        // which layer to use based on the options/URI provided to it.
        if (shellGlobalParams.gRPC) {
            // Create the gRPC client metadata.
            boost::optional<std::string> appname = parsedURI.getAppName();
            BSONObjBuilder bob;
            uassertStatusOK(DBClientSession::appendClientMetadata(
                appname.value_or(MongoURI::kDefaultTestRunnerAppName), &bob));
            auto metadataDoc = bob.obj();

            // Create the gRPC transport layer.
            transport::grpc::GRPCTransportLayer::Options grpcOpts;
            grpcOpts.enableEgress = true;
            grpcOpts.clientMetadata = metadataDoc.getObjectField(kMetadataDocumentName).getOwned();
            tls.push_back(std::make_unique<transport::grpc::GRPCTransportLayerImpl>(
                serviceContext, grpcOpts, nullptr));
        }
#endif

        serviceContext->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tls), asioLayer));

        auto tlPtr = serviceContext->getTransportLayerManager();
        uassertStatusOK(tlPtr->setup());
        uassertStatusOK(tlPtr->start());

#ifdef MONGO_CONFIG_SSL
        if (!awsIam::saslAwsClientGlobalParams.awsSessionToken.empty()) {
            parsedURI.setOptionIfNecessary("authmechanismproperties"s,
                                           std::string("AWS_SESSION_TOKEN:") +
                                               awsIam::saslAwsClientGlobalParams.awsSessionToken);
        }
#endif
        if (!oidcClientGlobalParams.oidcAccessToken.empty()) {
            parsedURI.setOptionIfNecessary("authmechanismproperties"s,
                                           str::stream() << "OIDC_ACCESS_TOKEN:"
                                                         << oidcClientGlobalParams.oidcAccessToken);
        }

        if (const auto authMechanisms = parsedURI.getOption("authMechanism")) {
            std::stringstream ss;
            ss << "DB.prototype._defaultAuthenticationMechanism = \""
               << str::escape(authMechanisms.value()) << "\";" << std::endl;
            mongo::shell_utils::dbConnect += ss.str();
        }

        if (const auto gssapiServiveName = parsedURI.getOption("gssapiServiceName")) {
            std::stringstream ss;
            ss << "DB.prototype._defaultGssapiServiceName = \""
               << str::escape(gssapiServiveName.value()) << "\";" << std::endl;
            mongo::shell_utils::dbConnect += ss.str();
        }

        if (!shellGlobalParams.nodb) {  // connect to db
            bool usingPassword = !shellGlobalParams.password.empty();

            if (mechanismRequiresPassword(parsedURI) &&
                (parsedURI.getUser().size() || shellGlobalParams.username.size())) {
                usingPassword = true;
            }

            if (usingPassword && parsedURI.getPassword().empty()) {
                if (!shellGlobalParams.password.empty()) {
                    parsedURI.setPassword(stdx::as_const(shellGlobalParams.password));
                } else {
                    parsedURI.setPassword(mongo::askPassword());
                }
            }

            if (parsedURI.getUser().empty() && !shellGlobalParams.username.empty()) {
                parsedURI.setUser(stdx::as_const(shellGlobalParams.username));
            }

            std::stringstream ss;
            if (mongo::serverGlobalParams.quiet.load()) {
                ss << "__quiet = true;" << std::endl;
            }

            ss << "db = connect( \"" << parsedURI.canonicalizeURIAsString()
               << "\", null, null, {api: " << getApiParametersJSON() << "});" << std::endl;

            if (shellGlobalParams.shouldRetryWrites || parsedURI.getRetryWrites()) {
                // If the --retryWrites cmdline argument or retryWrites URI param was specified,
                // then replace the global `db` object with a DB object started in a session. The
                // resulting Mongo connection checks its _retryWrites property.
                ss << "db = db.getMongo().startSession().getDatabase(db.getName());" << std::endl;
            }

            mongo::shell_utils::dbConnect += ss.str();
        }

        mongo::ScriptEngine::setConnectCallback(mongo::shell_utils::onConnect);
        mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
        mongo::getGlobalScriptEngine()->setJSHeapLimitMB(shellGlobalParams.jsHeapLimitMB);
        mongo::getGlobalScriptEngine()->setScopeInitCallback(mongo::shell_utils::initScope);
        mongo::getGlobalScriptEngine()->enableJavaScriptProtection(
            shellGlobalParams.javascriptProtection);

        if (shellGlobalParams.files.size() > 0) {
            boost::system::error_code ec;
            auto loadPath =
                boost::filesystem::canonical(shellGlobalParams.files[0], ec).parent_path().string();
            if (!ec) {
                mongo::getGlobalScriptEngine()->setLoadPath(loadPath);
            }
        }

        ScopeGuard poolGuard([] { ScriptEngine::dropScopeCache(); });

        std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());
        shellMainScope = scope.get();

        if (shellGlobalParams.runShell && !mongo::serverGlobalParams.quiet.load())
            std::cout << "type \"help\" for help" << std::endl;

        // Load and execute /etc/mongorc.js before starting shell
        std::string rcGlobalLocation;
#ifndef _WIN32
        rcGlobalLocation = "/etc/mongorc.js";
#else
        wchar_t programDataPath[MAX_PATH];
        if (S_OK == SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programDataPath)) {
            rcGlobalLocation = str::stream()
                << toUtf8String(programDataPath) << "\\MongoDB\\mongorc.js";
        }
#endif
        if (!rcGlobalLocation.empty() && ::mongo::shell_utils::fileExists(rcGlobalLocation)) {
            if (!scope->execFile(rcGlobalLocation, false, true)) {
                std::cout << "The \"" << rcGlobalLocation << "\" file could not be executed"
                          << std::endl;
            }
        }

        if (!shellGlobalParams.script.empty()) {
            mongo::shell_utils::MongoProgramScope s;
            if (!scope->exec(shellGlobalParams.script, "(shell eval)", false, true, false)) {
                std::cout << "exiting with code " << static_cast<int>(kEvalError) << std::endl;
                return kEvalError;
            }
            scope->exec("shellPrintHelper( __lastres__ );", "(shell2 eval)", true, true, false);
        }

        for (size_t i = 0; i < shellGlobalParams.files.size(); ++i) {
            mongo::shell_utils::MongoProgramScope s;

            if (shellGlobalParams.files.size() > 1)
                std::cout << "loading file: " << shellGlobalParams.files[i] << std::endl;

            if (!scope->execFile(shellGlobalParams.files[i], false, true)) {
                std::cout << "failed to load: " << shellGlobalParams.files[i] << std::endl;
                std::cout << "exiting with code " << static_cast<int>(kInputFileError) << std::endl;
                mongo::shell_utils::KillMongoProgramInstances(SIGKILL);
                return kInputFileError;
            }

            // If the test is using the mochalite framework, invoke the runner
            try {
                shell_utils::closeMochaStyleTestContext(*shellMainScope);
            } catch (std::exception&) {
                std::cout << "Failure detected from Mocha test runner" << std::endl;
                return kInputFileError;
            }

            // If the test began a GoldenTestContext, end it and compare actual/expected results.
            // NOTE: putting this in ~MongoProgramScope would call it at the end of each load(),
            // but we only want to call it once the original test file finishes.
            try {
                shell_utils::closeGoldenTestContext();
            } catch (const shell_utils::GoldenTestContextShellFailure& exn) {
                std::cout << "failed to load: " << shellGlobalParams.files[i] << std::endl;
                std::cout << exn.toString() << std::endl;
                exn.diff();
                std::cout << "exiting with code " << static_cast<int>(kInputFileError) << std::endl;
                return kInputFileError;
            }

            // Check if the process left any running child processes.
            std::vector<ProcessId> pids = mongo::shell_utils::getRunningMongoChildProcessIds();

            if (!pids.empty()) {
                std::cout << "terminating the following processes started by "
                          << shellGlobalParams.files[i] << ": ";
                std::copy(
                    pids.begin(), pids.end(), std::ostream_iterator<ProcessId>(std::cout, " "));
                std::cout << std::endl;

                // Some tests spawn child server processes that are expected to crash or otherwise
                // terminate uncleanly. These tests should set the
                // TestData.ignoreChildProcessErrorCode flag to true so that any nonzero error
                // codes do not cause the test to fail.
                //
                // The shell checks this flag here by executing a bit of JS to inspect
                // TestData.ignoreChildProcessErrorCode and return its value. If TestData is null or
                // does not have the ignoreChildProcessErrorCode field, then the JS returns false.
                // If TestData.ignoreChildProcessErrorCode has been explicitly set to true, then it
                // returns true; otherwise it returns false.
                //
                // TestData.ignoreChildProcessErrorCode is set to false by default.
                bool ignoreChildProcessErrorCode = false;
                StringData code =
                    "function() { return typeof TestData === 'object' && TestData !== null && "
                    "TestData.hasOwnProperty('ignoreChildProcessErrorCode') && "
                    "TestData.ignoreChildProcessErrorCode === true; }"_sd;
                shellMainScope->invokeSafe(code.data(), nullptr, nullptr);
                ignoreChildProcessErrorCode = shellMainScope->getBoolean("__returnValue");
                auto childProcessErrorCode = mongo::shell_utils::KillMongoProgramInstances();

                if (!ignoreChildProcessErrorCode) {
                    if (childProcessErrorCode != static_cast<int>(ExitCode::clean)) {
                        std::cout << "one or more child processes exited with an error during "
                                  << shellGlobalParams.files[i] << std::endl;
                        std::cout << "exiting with code "
                                  << static_cast<int>(kProcessTerminationError) << std::endl;
                        return kProcessTerminationError;
                    }
                } else {
                    std::cout << "Ignoring child process exit codes since "
                                 "TestData.ignoreChildProcessErrorCode is true"
                              << std::endl;
                }

                // Similarly, some tests spawn child server processes that are expected to not
                // terminate at all. These tests should set the TestData.ignoreUnterminatedProcesses
                // flag to true so that zombie processes do not cause the test to fail.
                //
                // The shell checks this flag here by executing a bit of JS to inspect
                // TestData.ignoreUnterminatedProcesses and return its value. If TestData is null or
                // does not have the ignoreUnterminatedProcesses field, then the JS returns false.
                // If TestData.ignoreUnterminatedProcesses has been explicitly set to true, then it
                // returns true; otherwise it returns false.
                //
                // TestData.ignoreUnterminatedProcesses is set to false by default.
                bool ignoreUnterminatedProcesses = false;
                code =
                    "function() { return typeof TestData === 'object' && TestData !== null && "
                    "TestData.hasOwnProperty('ignoreUnterminatedProcesses') && "
                    "TestData.ignoreUnterminatedProcesses === true; }"_sd;
                shellMainScope->invokeSafe(code.data(), nullptr, nullptr);
                ignoreUnterminatedProcesses = shellMainScope->getBoolean("__returnValue");

                if (!ignoreUnterminatedProcesses) {
                    std::cout << "exiting with a failure due to unterminated processes, "
                                 "a call to MongoRunner.stopMongod(), ReplSetTest#stopSet(), or "
                                 "ShardingTest#stop() may be missing from the test"
                              << std::endl;
                    std::cout << "exiting with code " << static_cast<int>(kUnterminatedProcess)
                              << std::endl;
                    return kUnterminatedProcess;
                } else {
                    std::cout << "Ignoring unterminated processes since "
                                 "TestData.ignoreUnterminatedProcesses is true"
                              << std::endl;
                }
            }

            // If any core dumps exist from the test that ran, then that means the test unexpectedly
            // left a core dump behind.
            pids = mongo::shell_utils::getRegisteredPidsHistory();
            std::cout << "All pids dead / alive (" << pids.size() << "): ";
            for (const auto pid : pids) {
                std::cout << pid << ", ";
            }
            std::cout << std::endl;

            // Iterate through files to see if we find a dump file containing a pid of a program
            // belonging to the test that ran.
            std::cout << "Searching for files in: " << shellMainScope->getBaseURL() << std::endl;
            auto files = mongo::shell_utils::ls(BSON("" << shellMainScope->getBaseURL()), nullptr);
            std::vector<std::string> coreDumpsFound;
            for (const auto& elem : files[""].Array()) {
                auto fileName = elem.String();
                for (const auto& pid : pids) {
                    if (fileName.find("dump_") != std::string::npos &&
                        fileName.find("." + pid.toString() + ".core") != std::string::npos) {
                        std::cout << "Found a core dump '" << fileName << "'" << std::endl;
                        coreDumpsFound.push_back(fileName);
                    }
                }
            }

            if (!coreDumpsFound.empty()) {
                auto code =
                    "function() { return typeof TestData === 'object' && TestData !== null && "
                    "TestData.hasOwnProperty('cleanUpCoreDumpsFromExpectedCrash') && "
                    "TestData.cleanUpCoreDumpsFromExpectedCrash === true; }"_sd;
                shellMainScope->invokeSafe(code.data(), nullptr, nullptr);
                bool cleanUpCoreDumpsFromExpectedCrash =
                    shellMainScope->getBoolean("__returnValue");

                if (!cleanUpCoreDumpsFromExpectedCrash) {
                    // We unexpectedly found core dumps for this test.
                    std::cout << "exiting with a failure due to finding core dumps unexpectedly, "
                                 "the variable TestData.cleanUpCoreDumpsFromExpectedCrash may be "
                                 "missing from the test."
                              << std::endl;
                    std::cout << "exiting with code " << static_cast<int>(kUnexpectedCoreDumpFound)
                              << std::endl;
                    return kUnexpectedCoreDumpFound;
                } else {
                    // If we expected to find core dumps, then clean the core dumps up.
                    for (const auto& dumpFile : coreDumpsFound) {
                        std::cout << "TestData.cleanUpCoreDumpsFromExpectedCrash is set; deleting "
                                     "core dump '"
                                  << dumpFile << "'" << std::endl;
                        mongo::shell_utils::removeFile(BSON("" << dumpFile), nullptr);
                    }
                }
            }
        }

        {
            const StringData parallelShellCode = "uncheckedParallelShellPidsString();"_sd;
            shellMainScope->invokeSafe(parallelShellCode.data(), nullptr, nullptr);
            std::string ret = shellMainScope->getString("__returnValue");
            if (!ret.empty()) {
                std::cout << "exiting due to parallel shells with unchecked return values. "
                             "When starting a parallel shell, always call the returned "
                             "function to ensure correct process cleanup and handling "
                             "of failed assertions."
                          << std::endl;
                std::cout << "pids of parallel shells: " << ret << std::endl;
                std::cout << "exiting with code " << static_cast<int>(kUnterminatedProcess)
                          << std::endl;
                return kUnterminatedProcess;
            }
        }

        if (shellGlobalParams.files.size() == 0 && shellGlobalParams.script.empty())
            shellGlobalParams.runShell = true;

        bool lastLineSuccessful = true;
        if (shellGlobalParams.runShell) {
            if (!mongo::serverGlobalParams.quiet.load())
                std::cout << "================\n"
                             "Warning: the \"mongo\" shell has been superseded by \"mongosh\",\n"
                             "which delivers improved usability and compatibility."
                             "The \"mongo\" shell has been deprecated and will be removed in\n"
                             "an upcoming release.\n"
                             "For installation instructions, see\n"
                             "https://docs.mongodb.com/mongodb-shell/install/\n"
                             "================"
                          << std::endl;

            mongo::shell_utils::MongoProgramScope s;
            // If they specify norc, assume it's not their first time
            bool hasMongoRC = shellGlobalParams.norc;
            std::string rcLocation;
            if (!shellGlobalParams.norc) {
#ifndef _WIN32
                if (getenv("HOME") != nullptr)
                    rcLocation = str::stream() << getenv("HOME") << "/.mongorc.js";
#else
                if (getenv("HOMEDRIVE") != nullptr && getenv("HOMEPATH") != nullptr)
                    rcLocation = str::stream()
                        << toUtf8String(_wgetenv(L"HOMEDRIVE"))
                        << toUtf8String(_wgetenv(L"HOMEPATH")) << "\\.mongorc.js";
#endif
                if (!rcLocation.empty() && ::mongo::shell_utils::fileExists(rcLocation)) {
                    hasMongoRC = true;
                    if (!scope->execFile(rcLocation, false, true)) {
                        std::cout
                            << "The \".mongorc.js\" file located in your home folder could not be "
                               "executed"
                            << std::endl;
                        std::cout << "exiting with code " << static_cast<int>(kMongorcError)
                                  << std::endl;
                        return kMongorcError;
                    }
                }
            }

            if (!hasMongoRC && isatty(fileno(stdin))) {
                std::cout
                    << "Welcome to the MongoDB shell.\n"
                       "For interactive help, type \"help\".\n"
                       "For more comprehensive documentation, see\n\thttps://docs.mongodb.com/\n"
                       "Questions? Try the MongoDB Developer Community Forums\n"
                       "\thttps://community.mongodb.com"
                    << std::endl;
                File f;
                f.open(rcLocation.c_str(), false);  // Create empty .mongorc.js file
            }

            if (!shellGlobalParams.nodb && !mongo::serverGlobalParams.quiet.load() &&
                isatty(fileno(stdin))) {
                scope->exec("shellHelper( 'show', 'startupWarnings' )",
                            "(shellwarnings)",
                            false,
                            true,
                            false);

                scope->exec("shellHelper( 'show', 'automationNotices' )",
                            "(automationnotices)",
                            false,
                            true,
                            false);

                scope->exec("shellHelper( 'show', 'nonGenuineMongoDBCheck' )",
                            "(nonGenuineMongoDBCheck)",
                            false,
                            true,
                            false);
            }

            shellHistoryInit();

            // Only run the prelude in interactive mode
            scope->execPrelude();

            std::string prompt;
            int promptType;

            while (1) {
                inMultiLine = false;
                gotInterrupted = false;

                promptType = scope->type("prompt");
                if (promptType == stdx::to_underlying(BSONType::string)) {
                    prompt = scope->getString("prompt");
                } else if ((promptType == stdx::to_underlying(BSONType::code)) &&
                           execPrompt(*scope, "prompt", prompt)) {
                } else if (execPrompt(*scope, "defaultPrompt", prompt)) {
                } else {
                    prompt = "> ";
                }

                char* line = shellReadline(prompt.c_str());

                char* linePtr = line;  // can't clobber 'line', we need to free() it later
                if (linePtr) {
                    while (linePtr[0] == ' ')
                        ++linePtr;
                    int lineLen = strlen(linePtr);
                    while (lineLen > 0 && linePtr[lineLen - 1] == ' ')
                        linePtr[--lineLen] = 0;
                }

                if (!linePtr || (strlen(linePtr) == 4 && strstr(linePtr, "exit"))) {
                    if (!mongo::serverGlobalParams.quiet.load())
                        std::cout << "bye" << std::endl;
                    if (line)
                        free(line);
                    break;
                }

                std::string code = linePtr;
                if (code == "exit" || code == "exit;") {
                    free(line);
                    break;
                }

                // Support idle session lifetime limits
                if (isSessionTimedOut()) {
                    std::cout << "Idle Connection Timeout: Shell session has expired" << std::endl;
                    if (line)
                        free(line);
                    break;
                }

                if (code == "cls") {
                    free(line);
                    linenoiseClearScreen();
                    continue;
                }

                if (code.size() == 0) {
                    free(line);
                    continue;
                }

                if (str::startsWith(linePtr, "edit ")) {
                    shellHistoryAdd(linePtr);

                    const char* s = linePtr + 5;  // skip "edit "
                    while (*s && ctype::isSpace(*s))
                        s++;

                    edit(s);
                    free(line);
                    continue;
                }

                gotInterrupted = false;
                code = finishCode(code);
                if (gotInterrupted) {
                    std::cout << std::endl;
                    free(line);
                    continue;
                }

                if (code.size() == 0) {
                    free(line);
                    break;
                }

                bool wascmd = false;
                {
                    std::string cmd = linePtr;
                    std::string::size_type firstSpace;
                    if ((firstSpace = cmd.find(' ')) != std::string::npos)
                        cmd = cmd.substr(0, firstSpace);

                    if (cmd.find('\"') == std::string::npos) {
                        try {
                            lastLineSuccessful = scope->exec(
                                std::string("__iscmd__ = shellHelper[\"") + cmd + "\"];",
                                "(shellhelp1)",
                                false,
                                true,
                                true);
                            if (scope->getBoolean("__iscmd__")) {
                                lastLineSuccessful =
                                    scope->exec(std::string("shellHelper( \"") + cmd + "\" , \"" +
                                                    code.substr(cmd.size()) + "\");",
                                                "(shellhelp2)",
                                                false,
                                                true,
                                                false);
                                wascmd = true;
                            }
                        } catch (std::exception& e) {
                            std::cout << "error2:" << e.what() << std::endl;
                            wascmd = true;
                            lastLineSuccessful = false;
                        }
                    }
                }

                if (!wascmd) {
                    try {
                        lastLineSuccessful =
                            scope->exec(code.c_str(), "(shell)", false, true, false);
                        if (lastLineSuccessful) {
                            scope->exec(
                                "shellPrintHelper( __lastres__ );", "(shell2)", true, true, false);
                        }
                    } catch (std::exception& e) {
                        std::cout << "error:" << e.what() << std::endl;
                        lastLineSuccessful = false;
                    }
                }

                shellHistoryAdd(code.c_str());
                free(line);
            }

            shellHistoryDone();
        }

        return (lastLineSuccessful ? 0 : 1);

    } catch (DBException& e) {
        std::cout << "exception: " << e.what() << std::endl;
        std::cout << "exiting with code " << static_cast<int>(kDBException) << std::endl;
        return kDBException;
    }
}

}  // namespace mongo
