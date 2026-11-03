/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

// This implements the Landlock sandbox initialization (Linux only) for mongod and mongos:
// It defines the server's own Landlock policy and the startup initializers that apply it.

#if defined(__linux__)

#include "mongo/db/landlock_init.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/initialize_server_global_state_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/landlock/landlock.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/stacktrace_somap.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <yaml-cpp/yaml.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {
namespace {
// How hard the server tries to sandbox itself (security.landlock.mode).
//
// kEnforce and kBestEffort both apply the strongest ruleset the running
// kernel's Landlock ABI supports, and differ only in what happens when
// Landlock is not available:
// kEnforce - server exits immediately,
// kBestEffort - server starts unsandboxed.
enum class LandlockMode { kDisabled, kBestEffort, kEnforce };

StatusWith<LandlockMode> parseLandlockMode(const std::string& value) {
    if (str::equalCaseInsensitive(value, "disabled")) {
        return LandlockMode::kDisabled;
    }
    if (str::equalCaseInsensitive(value, "bestEffort")) {
        return LandlockMode::kBestEffort;
    }
    if (str::equalCaseInsensitive(value, "enforce")) {
        return LandlockMode::kEnforce;
    }
    return Status(ErrorCodes::BadValue,
                  str::stream() << "security.landlock.mode expects 'enforce', 'bestEffort' or "
                                   "'disabled'; got '"
                                << value << "'");
}

// The canonical spelling, for reporting the mode back through serverStatus.
std::string_view landlockModeName(LandlockMode mode) {
    switch (mode) {
        case LandlockMode::kDisabled:
            return "disabled";
        case LandlockMode::kBestEffort:
            return "bestEffort";
        case LandlockMode::kEnforce:
            return "enforce";
    }
    MONGO_UNREACHABLE;
}

// Back the "landlock" serverStatus section (see LandlockServerStatusSection):
// the sandbox mode in effect, the Landlock ABI version probed from the running
// kernel (0 when the kernel lacks Landlock or has it disabled), both reported
// even when the sandbox is disabled, whether the sandbox is actually enforced
// ("active"), and the filesystem access-right masks the enforced ruleset
// handles and had to degrade (meaningful only while active).
//
// Deliberately not atomic: written once, from the single-threaded startup
// initializers that build and enforce the sandbox, before any thread that could
// read them (command threads serving serverStatus) is spawned -- thread creation
// establishes the necessary happens-before. Must become atomic if enforcement
// ever moves out of single-threaded startup.
LandlockMode gLandlockMode = LandlockMode::kDisabled;
int gLandlockAbi = 0;
// The policy version in effect: security.landlock.policyVersion, or the latest this binary
// implements when unset.
int gLandlockPolicyVersion = LandlockPolicy::latest().version();
bool gLandlockActive = false;
uint64_t gLandlockHandledFsAccess = 0;
uint64_t gLandlockDegradedFsAccess = 0;

// The ruleset the policy is assembled into, between the
// BeginLandlockSandboxInitialization and EndLandlockSandboxInitialization
// initializers; see globalLandlockRuleset() for the contract and the same
// single-threaded-startup rationale as the globals above. Null means no sandbox
// is being built: either sandboxing is disabled or this host cannot enforce it.
std::unique_ptr<LandlockRuleset> gLandlockRuleset;

}  // namespace

LandlockRuleset* globalLandlockRuleset() {
    return gLandlockRuleset.get();
}

void addGlobalLandlockRules(const std::vector<LandlockFilesystemRule>& rules) {
    invariant(gLandlockRuleset,
              "Attempted to contribute to the Landlock policy with no ruleset being assembled; a "
              "contributor must declare BeginLandlockSandboxInitialization as a prerequisite and "
              "EndLandlockSandboxInitialization as a dependent, and check globalLandlockRuleset()");

    for (const auto& rule : rules) {
        // An empty path marks a feature not in use under the current
        // configuration (no UNIX socket, no TLS, ...); nothing to grant.
        if (rule.path().empty()) {
            continue;
        }
        if (Status status = gLandlockRuleset->addPathRule(rule); !status.isOK()) {
            LOGV2_FATAL(13118809,
                        "Failed to add Landlock rule",
                        "path"_attr = rule.path(),
                        "error"_attr = status);
        }
    }
}

namespace landlock_policy {
namespace {

// Every helper below resolves a configured path against the specified PathBase. See PathBase in
// landlock_init.h for which base a given path needs.
boost::filesystem::path canonicalStartupPath(const std::string& path, PathBase base) {
    boost::filesystem::path basePath;
    if (base == PathBase::kProcessCwd) {
        boost::system::error_code ec;
        basePath = boost::filesystem::current_path(ec);
        if (ec) {
            LOGV2_FATAL(13444800,
                        "Failed to obtain the process current working directory path",
                        "error"_attr = ec.message());
        }
    } else {
        basePath = boost::filesystem::path(serverGlobalParams.cwd);
    }
    try {
        return canonicalizePathForLandlock(path, basePath);
    } catch (const DBException& ex) {
        LOGV2_FATAL(13444801,
                    "Failed to resolve a path for Landlock",
                    "path"_attr = path,
                    "base"_attr = basePath.string(),
                    "error"_attr = ex);
    }
}

std::vector<std::string> startupOptionList(const std::string& key) {
    const auto& params = optionenvironment::startupOptionsParsed;
    return params.count(key) ? params[key].as<std::vector<std::string>>()
                             : std::vector<std::string>{};
}
}  // namespace

// A configured path resolved for a rule (see canonicalStartupPath). Empty stays
// empty (option not in use).
std::string resolvedConfigPath(const std::string& path, PathBase base) {
    return canonicalStartupPath(path, base).string();
}

// The parent directory of a configured file path, for files the server
// creates, replaces or re-reads after the sandbox engages (log file, pid
// file, rotated credentials). A rule cannot be attached to a file that does
// not exist yet, and Landlock rules bind to the inode, so a grant on the file
// itself would go stale on the first rename-and-recreate rotation; a grant on
// the directory covers the entry regardless of which inode backs it (Landlock
// checks at open(2) time). Empty stays empty (option not in use).
std::string configParentDir(const std::string& path, PathBase base) {
    return canonicalStartupPath(path, base).parent_path().string();
}

// A configured directory the server creates itself when missing (e.g. an FTDC
// directory override). A rule needs an existing filesystem object to bind to,
// so when the directory does not exist yet the grant goes on its parent
// instead, which also permits the creation. One level only: if the parent is
// missing too, the rule is skipped (see addPathRule) and the operator must
// grant an existing ancestor via additional path rules rather than this
// policy silently widening towards "/". Empty stays empty.
std::string configDirCreatedIfMissing(const std::string& path, PathBase base) {
    const auto resolved = canonicalStartupPath(path, base);
    return (boost::filesystem::exists(resolved) ? resolved : resolved.parent_path()).string();
}

// The value of a setParameter given at startup, or empty when not set. Server
// parameters live with their owning subsystem; reading the parsed option
// instead keeps a policy free of link dependencies on those subsystems.
std::string setParameterStartupValue(const std::string& name) {
    const auto& params = optionenvironment::startupOptionsParsed;
    if (!params.count("setParameter")) {
        return {};
    }
    const auto parameters = params["setParameter"].as<std::map<std::string, std::string>>();
    const auto it = parameters.find(name);
    return it != parameters.end() ? it->second : std::string{};
}

// A string-valued startup option, read from the parsed options rather than from
// the global its subsystem parses it into, so a policy needs no link dependency
// on the module that owns the option -- including options only some binaries
// register at all (e.g. the enterprise audit log path). Unregistered or unset
// gives empty.
std::string startupOptionValue(const std::string& key) {
    const auto& params = optionenvironment::startupOptionsParsed;
    return params.count(key) ? params[key].as<std::string>() : std::string{};
}
}  // namespace landlock_policy

namespace {

using namespace landlock_policy;

std::vector<LandlockFilesystemRule> dynamicFilesystemRules() {
    std::vector<LandlockFilesystemRule> rules;

    // Unless the cwd is the root directory (e.g. mongodb running as a systemd
    // service), grant limited write access to it so the server can write core dumps.
    if (auto path = resolvedConfigPath(".", PathBase::kProcessCwd); path != "/") {
        rules.push_back(LandlockFilesystemRule::readOnly(
            path, LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_REG));
    }

    // Add read-only permission to this executable itself and its linked .so files so that
    // libunwind can read them. This assumes the first entry in the globalSharedObjectMapInfo
    // object contains the path to this executable itself (read from /proc/self/exe).
    if (const auto& soMap = globalSharedObjectMapInfo().obj().getField("somap");
        soMap.type() == BSONType::array) {
        for (auto&& entry : soMap.Array()) {
            if (!entry.isABSONObj()) {
                continue;
            }
            // note: empty rule if "path" field does not exist.
            // if "path" is relative, resolve it based on where the symbolizer will access it,
            // which is relative to the process cwd.
            rules.push_back(LandlockFilesystemRule::readOnly(resolvedConfigPath(
                std::string(entry.Obj().getStringField("path")), PathBase::kProcessCwd)));
        }
    }

    // A --bind_ip entry may be a UNIX domain socket path rather than an address, mixed freely
    // with addresses in the same list. Such an entry is bound verbatim, at the path given, which
    // means a relative path is resolved relative to the post-fork cwd ('/').
    // The parent directory gets the grant, for the same reason as the log and pid files: the
    // socket does not exist when the ruleset is built, and binding first unlinks any stale one,
    // so a rule on the socket itself would have nothing to attach to.
    for (const auto& bindAddr : serverGlobalParams.bind_ips) {
        if (isUnixDomainSocket(bindAddr)) {
            rules.push_back(LandlockFilesystemRule::readWrite(
                configParentDir(bindAddr, PathBase::kProcessCwd)));
        }
    }

    // Query extensions
    if (getTestCommandsEnabled()) {
        rules.push_back(LandlockFilesystemRule::readOnly(
            resolvedConfigPath("src/mongo/db/extension/test_examples", PathBase::kProcessCwd)));
    }

    if (!serverGlobalParams.extensionsConfigPath.empty()) {
        const auto confDir = boost::filesystem::path(serverGlobalParams.extensionsConfigPath);

        for (const auto& name : serverGlobalParams.extensions) {
            const auto confPath = confDir / (name + ".conf");
            try {
                const auto root = YAML::LoadFile(confPath.string());
                if (const auto& sharedLibraryPath = root["sharedLibraryPath"]) {
                    auto soPath = sharedLibraryPath.as<std::string>();
                    rules.push_back(LandlockFilesystemRule::readOnly(
                        resolvedConfigPath(soPath, PathBase::kProcessCwd)));

                    rules.push_back(LandlockFilesystemRule::readOnly(
                        resolvedConfigPath(soPath + ".sig", PathBase::kProcessCwd)));
                }
            } catch (const YAML::Exception&) {
                continue;  // ignore invalid extension configs
            }
        }
    }
    return rules;
}

/**
 * The filesystem policy for mongod and mongos: every hierarchy the server
 * needs, as narrowly as feasible. An entry with an empty path describes a
 * feature not in use under the current configuration and grants nothing;
 * likewise a path absent on this system is skipped when added (see
 * addPathRule). Paths the server merely probes for optional metadata (e.g.
 * /etc/lsb-release for OS info) are deliberately unlisted: those reads fail
 * gracefully.
 */
std::vector<LandlockFilesystemRule> staticFilesystemRules() {
    // mongos never touches the storage layer; every other flavor of server
    // (including a mongod embedding a router) needs its data directory.
    const bool isRouterOnly =
        serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer);

    return {
        // Shared libraries and hosted binaries, notably glibc NSS modules
        // dlopen()'d lazily for name resolution. Landlock's EXECUTE right only
        // gates execve()-style execution, not mmap'ing a library, so read-only
        // suffices for dlopen().
        LandlockFilesystemRule::readOnly("/usr"),
        LandlockFilesystemRule::readOnly("/lib"),
        LandlockFilesystemRule::readOnly("/lib64"),

        // System configuration: library loading, name resolution (DNS matters
        // for replica set and sharded topologies), user/group lookups, TLS
        // trust stores, timezones. The whole directory rather than the handful
        // of files actually read, because files like resolv.conf are replaced
        // by rename (DHCP) and a per-file grant would go stale with the inode.
        // Reads that DAC forbids (e.g. /etc/shadow) remain forbidden; Landlock
        // only ever subtracts rights.
        LandlockFilesystemRule::readOnly("/etc"),

        // Where /etc/resolv.conf is a symlink into the resolver daemon's
        // runtime directory (systemd-resolved, resolvconf), the /etc grant
        // does not cover it: Landlock checks the resolved target, not the
        // symlink. Without this, DNS fails and replica-set self-identification
        // ("no host maps to this node") breaks. Absent directories are skipped.
        LandlockFilesystemRule::readOnly("/run/systemd/resolve"),
        LandlockFilesystemRule::readOnly("/run/resolvconf"),

        // Kernel and process introspection: FTDC and ProcessInfo read widely
        // under /proc (cpuinfo, meminfo, diskstats, self/*) and /sys (memory,
        // NUMA, THP and block device state).
        LandlockFilesystemRule::readOnly("/proc"),
        LandlockFilesystemRule::readOnly("/sys"),

        // glibc's pthread_setname_np() writes the thread name to
        // /proc/self/task/<tid>/comm for every thread spawned after the
        // sandbox engages.
        //
        // Pinned, because on procfs this path can come to resolve to a different inode, so without
        // it, the grant quietly stops applying partway through the process's life.
        LandlockFilesystemRule::readWrite(std::string("/proc/self/task")).withPinnedPath(),

        // /dev/null is opened read-write by various subsystems at runtime;
        // /dev/urandom backs SecureRandom when getrandom(2) is unavailable.
        LandlockFilesystemRule::readWrite("/dev/null"),
        LandlockFilesystemRule::readOnly("/dev/urandom"),

        // In case there is a call to tmpfile() somewhere
        LandlockFilesystemRule::readWrite("/tmp"),

        // The data directory: WiredTiger's files, the lock file, and FTDC's
        // diagnostic.data live beneath it.
        // mongod_options.cpp:storeMongodOptions() already prepends the startup cwd to a
        // non-absolute dbpath in case of a forked server, so the PathBase doesn't matter.
        // Grants REFER on top of readWrite, because FCBIS moves files from the '.initialsync'
        // subdirectory up into dbpath itself. Renames that reparent a file require REFER.
        LandlockFilesystemRule::readWrite(
            isRouterOnly ? ""
                         : resolvedConfigPath(storageGlobalParams.dbpath, PathBase::kProcessCwd),
            LANDLOCK_ACCESS_FS_REFER),

        // The directory the UNIX domain socket (mongodb-<port>.sock) is
        // created in and unlinked from; net.unixDomainSocket.pathPrefix,
        // default /tmp.
        LandlockFilesystemRule::readWrite(
            serverGlobalParams.noUnixSocket
                ? ""
                : resolvedConfigPath(serverGlobalParams.socket, PathBase::kProcessCwd)),

        // Proxy-protocol ingress (net.proxyPort) creates and unlinks its own
        // UNIX domain sockets under this prefix directory.
        LandlockFilesystemRule::readWrite(
            resolvedConfigPath(serverGlobalParams.proxySocketPrefix, PathBase::kProcessCwd)),

        // FTDC's directory defaults beneath dbpath (mongod) or beside the log
        // file (mongos), both covered by other rules; an explicit override
        // needs its own grant. FTDC creates the directory when missing.
        LandlockFilesystemRule::readWrite(configDirCreatedIfMissing(
            setParameterStartupValue("diagnosticDataCollectionDirectoryPath"),
            PathBase::kProcessCwd)),

        // Traffic recording writes a per-recording subdirectory of files under
        // trafficRecordingDirectory, on the startTrafficRecording command. The
        // directory must already exist (the parameter's validator requires it),
        // so the grant lands on it directly rather than on its parent. Unset
        // means recording is unavailable and nothing is granted. Note this is
        // the value configured at startup: the parameter is also settable at
        // runtime, but the ruleset is fixed before any command can run, so
        // repointing it outside this grant leaves recording unable to write.
        LandlockFilesystemRule::readWrite(resolvedConfigPath(
            setParameterStartupValue("trafficRecordingDirectory"), PathBase::kProcessCwd)),

        // OpenTelemetry's file exporters.
        LandlockFilesystemRule::readWrite(resolvedConfigPath(
            setParameterStartupValue("openTelemetryMetricsDirectory"), PathBase::kProcessCwd)),
        LandlockFilesystemRule::readWrite(resolvedConfigPath(
            setParameterStartupValue("opentelemetryTraceDirectory"), PathBase::kProcessCwd)),
        LandlockFilesystemRule::readWrite(
            resolvedConfigPath(setParameterStartupValue("openTelemetryPrometheusMetricsDirectory"),
                               PathBase::kProcessCwd)),
        // This setParameter names the file itself, so its parent gets the grant:
        // a ".tmp" sibling is created and renamed beside it, and the target file is
        // replaced rather than rewritten in place.
        LandlockFilesystemRule::readWrite(configParentDir(
            setParameterStartupValue("openTelemetryPrometheusMetricsPath"), PathBase::kProcessCwd)),

        // The time zone database (processManagement.timeZoneInfo) is loaded
        // during startup, after the sandbox engages.
        LandlockFilesystemRule::readOnly(
            resolvedConfigPath(serverGlobalParams.timeZoneInfoPath, PathBase::kProcessCwd)),

        // Log, pid and (test-only) backtrace files are created, rewritten and
        // rotated after the sandbox engages, so their parent directories get
        // the grant; the files themselves may not exist yet. This also covers
        // mongos's FTDC directory, which defaults to a sibling of the log
        // file.
        LandlockFilesystemRule::readWrite(
            configParentDir(serverGlobalParams.logpath, PathBase::kStartupCwd)),
        LandlockFilesystemRule::readWrite(
            configParentDir(serverGlobalParams.pidFile, PathBase::kProcessCwd)),
        LandlockFilesystemRule::readWrite(configParentDir(
            initialize_server_global_state::gBacktraceLogFile, PathBase::kStartupCwd)),

        // Credential files are re-read on rotation, which conventionally
        // replaces them by rename, so the containing directory gets the grant
        // (same inode rationale as configParentDir).
        LandlockFilesystemRule::readOnly(
            configParentDir(serverGlobalParams.keyFile, PathBase::kProcessCwd)),
        LandlockFilesystemRule::readOnly(
            configParentDir(sslGlobalParams.sslPEMKeyFile, PathBase::kProcessCwd)),
        LandlockFilesystemRule::readOnly(
            configParentDir(sslGlobalParams.sslClusterFile, PathBase::kProcessCwd)),
        LandlockFilesystemRule::readOnly(
            configParentDir(sslGlobalParams.sslCAFile, PathBase::kProcessCwd)),
        LandlockFilesystemRule::readOnly(
            configParentDir(sslGlobalParams.sslClusterCAFile, PathBase::kProcessCwd)),
        LandlockFilesystemRule::readOnly(
            configParentDir(sslGlobalParams.sslCRLFile, PathBase::kProcessCwd)),

        // The Diffie-Hellman parameters file (opensslDiffieHellmanParameters)
        // is read wherever an SSL context is built, which includes the rebuild
        // that rotateCertificates performs, so it too outlives startup and gets
        // the same directory grant as the files above.
        LandlockFilesystemRule::readOnly(
            configParentDir(sslGlobalParams.sslPEMTempDHParam, PathBase::kProcessCwd)),

    // Paths accessed for loading query extensions
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
        LandlockFilesystemRule::readOnly(resolvedConfigPath(
            serverGlobalParams.extensionsSignaturePublicKeyPath, PathBase::kProcessCwd)),
#endif
        LandlockFilesystemRule::readOnly(
            resolvedConfigPath(serverGlobalParams.extensionsConfigPath, PathBase::kProcessCwd)),
    };
}

/**
 * Grants the hierarchies the operator declared in
 * security.landlock.additionalPathRules, for paths the server has no way to derive --
 * a site-managed CA bundle, say, or a directory some feature reads. Absent or empty,
 * this grants nothing.
 *
 * One entry is exactly one rule. Unlike security.clusterIpSourceAllowlist and
 * processManagement.loadExtensions, entries are NOT split on commas: a path
 * can contain commas. The separator is the YAML list element, or a repeat of the command-line
 * option (the option composes, so entries from both sources accumulate).
 *
 * Syntax was already checked when the options were parsed
 * (validateLandlockAdditionalPathRules), so reaching a parse failure here would mean the
 * validator and the parser disagree.
 */
void registerAdditionalPathRules(LandlockRuleset& ruleset) {
    const auto entries = startupOptionList("security.landlock.additionalPathRules");
    if (entries.empty()) {
        return;
    }

    // These widen the sandbox past what the server asked for, so the whole set is
    // logged before any of it is applied: the individual grants that follow are
    // indistinguishable from derived ones in the log.
    LOGV2(13214200,
          "Landlock: granting additional path rules from the server configuration",
          "additionalPathRules"_attr = entries);

    for (const auto& entry : entries) {
        auto swRule = LandlockFilesystemRule::fromConfigString(entry);
        if (!swRule.isOK()) {
            LOGV2_FATAL(13214201,
                        "Failed to parse a configured additional Landlock path rule",
                        "additionalPathRule"_attr = entry,
                        "error"_attr = swRule.getStatus());
        }
        const auto& rule = swRule.getValue();

        // addPathRule() grants nothing for a path that is absent, which is right for
        // the derived rules -- they enumerate every path any configuration could need,
        // and most hosts have only some of them -- but wrong here. The operator named
        // this path, so there is a mistake in the configuration.
        // The non-throwing exists() overload is required: a filesystem_error
        // escaping this initializer would abort with no usable diagnostic.
        boost::system::error_code ec;
        if (!boost::filesystem::exists(rule.path(), ec)) {
            uasserted(13214202,
                      str::stream() << "Invalid security.landlock.additionalPathRules entry: '"
                                    << entry << "': the path specified could not be read ("
                                    << (ec ? ec.message() : "no such file or directory") << ")");
        }

        if (Status status = ruleset.addPathRule(rule, false /*internal*/); !status.isOK()) {
            LOGV2_FATAL(13214203,
                        "Failed to add a configured additional Landlock path rule",
                        "additionalPathRule"_attr = entry,
                        "error"_attr = status);
        }
    }
}

// Opens the sandbox for assembly as directed by --landlockMode /
// security.landlock.mode (default: disabled): creates the ruleset every later
// contributor adds to, and grants the community policy. Never enforces an
// incomplete policy -- either the whole policy applies or the process dies (see
// addGlobalLandlockRules).
//
// Runs after the known file-touching initializers (ServerLogRedirection opens
// the log and backtrace files; ForkServer reopens the standard streams and
// chdirs), because the policy is derived from paths they finalize. Other
// pre-"default" initializers stay unordered against this one on purpose: test
// builds shuffle them, so an unlisted file-toucher fails loudly in testing and
// gets added as a prerequisite here. Note the shuffle can also land one between
// this initializer and EndLandlockSandboxInitialization, where it runs
// unsandboxed and says nothing -- the window is meant to hold policy
// contributors only, and nothing else may rely on being in it.
MONGO_INITIALIZER_GENERAL(BeginLandlockSandboxInitialization,
                          ("EndStartupOptionHandling", "ForkServer", "ServerLogRedirection"),
                          ())
(InitializerContext*) {
    const auto& params = optionenvironment::startupOptionsParsed;
    if (auto value = params["security.landlock.mode"]; !value.isEmpty()) {
        gLandlockMode = uassertStatusOK(parseLandlockMode(value.as<std::string>()));
    }

    // The version is validated at option parsing, so a failure here would mean the validator
    // and this lookup disagree. Requesting a version newer than this binary implements is
    // refused rather than quietly downgraded to what it has.
    if (auto value = params["security.landlock.policyVersion"]; !value.isEmpty()) {
        gLandlockPolicyVersion = value.as<int>();
    }
    const LandlockPolicy& policy =
        *uassertStatusOK(LandlockPolicy::forVersion(gLandlockPolicyVersion));

    // Probed even when disabled, so monitoring can tell a host that could enforce
    // the sandbox from one that never could.
    const auto swAbi = landlockAbiVersion();
    gLandlockAbi = swAbi.isOK() ? static_cast<int>(swAbi.getValue()) : 0;

    if (gLandlockMode == LandlockMode::kDisabled) {
        LOGV2_WARNING(13118814, "Skipping Landlock initialization: sandboxing is not enabled");
        return;
    }

    // A failed probe means this host cannot enforce Landlock at all: no support in
    // the kernel (ENOSYS), the LSM left out of the boot-time stack (EOPNOTSUPP).
    // A host without Landlock never gets a ruleset, so every later contributor
    // sees a null globalLandlockRuleset() and grants nothing.
    if (!swAbi.isOK()) {
        if (gLandlockMode == LandlockMode::kEnforce) {
            LOGV2_ERROR(13253500,
                        "Refusing to start: this host cannot enforce the Landlock filesystem "
                        "sandbox and security.landlock.mode requires it",
                        "mode"_attr = landlockModeName(gLandlockMode),
                        "error"_attr = swAbi.getStatus());
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "security.landlock.mode is '" << landlockModeName(gLandlockMode)
                          << "' but this host cannot enforce the Landlock filesystem "
                             "sandbox: "
                          << swAbi.getStatus().reason());
        }
        // best effort run server without Landlock
        LOGV2_WARNING(13253501,
                      "Continuing without Landlock filesystem sandboxing: this host cannot "
                      "enforce it, so the server's filesystem access is unrestricted",
                      "mode"_attr = landlockModeName(gLandlockMode),
                      "error"_attr = swAbi.getStatus());
        return;
    }

    // Landlock is available here. A failure from this point is the policy failing to apply,
    // which is fatal rather than something a mode gets to tolerate.
    auto swRuleset = LandlockRuleset::create(policy);
    if (!swRuleset.isOK()) {
        LOGV2_FATAL(
            13118806, "Failed to create Landlock ruleset", "error"_attr = swRuleset.getStatus());
    }
    gLandlockRuleset = std::move(swRuleset.getValue());

    LOGV2(13118808,
          "Applying Landlock filesystem sandbox",
          "policyVersion"_attr = policy.version(),
          "abiVersion"_attr = gLandlockRuleset->abiVersion());

    addGlobalLandlockRules(staticFilesystemRules());
    addGlobalLandlockRules(dynamicFilesystemRules());
}

// The point of no return: enforces whatever policy the initializers above
// assembled, and reports it. Gated on "default", so everything in that group --
// which is the rest of startup -- is guaranteed to run sandboxed, while every
// contributor to the policy must run before this one (see globalLandlockRuleset).
//
// Does nothing when no ruleset was assembled: sandboxing is disabled, or this
// host cannot enforce it. Both have already been logged.
MONGO_INITIALIZER_GENERAL(EndLandlockSandboxInitialization,
                          ("BeginLandlockSandboxInitialization"),
                          ("default"))
(InitializerContext*) {
    if (!gLandlockRuleset || gLandlockMode == LandlockMode::kDisabled) {
        return;
    }
    auto& ruleset = *gLandlockRuleset;

    registerAdditionalPathRules(ruleset);

    if (ruleset.abiVersion() < 7) {
        LOGV2_WARNING(13118810,
                      "Landlock ABI does not support audit logging of denials (requires ABI 7, "
                      "Linux 6.15+); denied operations will not appear in the kernel audit log",
                      "abiVersion"_attr = ruleset.abiVersion());
    }

    if (Status status = ruleset.restrictSelf(); !status.isOK()) {
        LOGV2_FATAL(13118811, "Failed to enforce Landlock ruleset", "error"_attr = status);
    }

    gLandlockActive = true;
    gLandlockHandledFsAccess = ruleset.handledFsAccess();
    gLandlockDegradedFsAccess = ruleset.degradedFsAccess();

    // The sandbox is now permanently in force. The handled arrays list only the
    // rights the running kernel actually restricts; rights that were requested
    // but unavailable on this ABI appear in degradedRights. Network and scope
    // restrictions are not handled yet (this ruleset is filesystem-only), so
    // those arrays are empty.
    BSONObjBuilder handledRights;
    handledRights.append("fs", LandlockRuleset::fsAccessRightNames(ruleset.handledFsAccess()));
    handledRights.append("net", std::vector<std::string>{});
    handledRights.append("scope", std::vector<std::string>{});
    LOGV2(13118812,
          "Landlock ruleset applied",
          "policyVersion"_attr = gLandlockPolicyVersion,
          "abiVersion"_attr = ruleset.abiVersion(),
          "handledAccessRights"_attr = handledRights.obj(),
          "degradedRights"_attr = LandlockRuleset::fsAccessRightNames(ruleset.degradedFsAccess()));
}

// Read-only diagnostic for monitoring and tests:
//
//   mode:                 the security.landlock.mode option ("enforce",
//                         "bestEffort" or "disabled")
//   active:               whether the sandbox is actually enforced
//   policyVersion:        the MongoDB Landlock policy version in effect
//                         (security.landlock.policyVersion, else the latest
//                         this binary implements), reported even when disabled
//   latestPolicyVersion:  the newest policy version this binary implements
//   abiVersion:           Landlock ABI probed from the running kernel (0 when
//                         the kernel lacks Landlock or has it disabled),
//                         reported even when the sandbox is disabled
//   handledAccessRights:  rights the enforced ruleset denies by default,
//                         per rule type ("fs"); present only when active
//   degradedAccessRights: requested rights this kernel's ABI cannot restrict,
//                         per rule type ("fs"); present only when active
class LandlockServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext*, const BSONElement&) const override {
        BSONObjBuilder builder;
        builder.append("mode", landlockModeName(gLandlockMode));
        builder.append("active", gLandlockActive);
        builder.append("policyVersion", gLandlockPolicyVersion);
        builder.append("latestPolicyVersion", LandlockPolicy::latest().version());
        builder.append("abiVersion", gLandlockAbi);
        if (gLandlockActive) {
            {
                BSONObjBuilder handled(builder.subobjStart("handledAccessRights"));
                handled.append("fs", LandlockRuleset::fsAccessRightNames(gLandlockHandledFsAccess));
            }
            {
                BSONObjBuilder degraded(builder.subobjStart("degradedAccessRights"));
                degraded.append("fs",
                                LandlockRuleset::fsAccessRightNames(gLandlockDegradedFsAccess));
            }
        }
        return builder.obj();
    }
};
auto& landlockServerStatusSection =
    *ServerStatusSectionBuilder<LandlockServerStatusSection>("landlock").forShard().forRouter();

}  // namespace
}  // namespace mongo

#endif  // defined(__linux__)
