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

// Landlock self-sandboxing for mongod and mongos (Linux only).
//
// Landlock is a Linux LSM that lets an unprivileged process irreversibly drop
// its own ambient filesystem rights. When enabled via --landlock
// (security.landlock.enabled), the server declares the set of path hierarchies it
// legitimately needs at startup (e.g. dbPath read-write, system libraries
// read-only) and the kernel denies every other filesystem access for the
// lifetime of the process and its descendants.
//
// The Landlock uAPI (struct layouts, access-right bits, syscall numbers) is
// defined locally in this file rather than by including <linux/landlock.h>:
// toolchain sysroot headers routinely lag the kernel we actually run on, and
// the uAPI is append-only and stable by contract, so mirroring it is safe.
// Every constant is guarded by #ifndef so a definition supplied by some other
// header always wins. The running kernel's Landlock ABI version, probed by
// LandlockRuleset, decides which of these rights are actually enforced.

#if defined(__linux__)

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

// Landlock uAPI constants, mirrored from <linux/landlock.h>. Values are part
// of the kernel's stable uAPI and never change; new rights are only ever
// appended (with a new ABI version). #ifndef keeps these compatible with any
// header that may already define them.

// The syscall numbers are identical on every architecture (the Landlock
// syscalls postdate the unified syscall table, Linux 5.13).
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

// landlock_create_ruleset() flag: query the kernel's Landlock ABI version
// instead of creating a ruleset.
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif

// Filesystem access rights (landlock_ruleset_attr.handled_access_fs and
// landlock_path_beneath_attr.allowed_access), with the ABI that introduced
// each right past the initial set.
#ifndef LANDLOCK_ACCESS_FS_EXECUTE
#define LANDLOCK_ACCESS_FS_EXECUTE (1ULL << 0)
#endif
#ifndef LANDLOCK_ACCESS_FS_WRITE_FILE
#define LANDLOCK_ACCESS_FS_WRITE_FILE (1ULL << 1)
#endif
#ifndef LANDLOCK_ACCESS_FS_READ_FILE
#define LANDLOCK_ACCESS_FS_READ_FILE (1ULL << 2)
#endif
#ifndef LANDLOCK_ACCESS_FS_READ_DIR
#define LANDLOCK_ACCESS_FS_READ_DIR (1ULL << 3)
#endif
#ifndef LANDLOCK_ACCESS_FS_REMOVE_DIR
#define LANDLOCK_ACCESS_FS_REMOVE_DIR (1ULL << 4)
#endif
#ifndef LANDLOCK_ACCESS_FS_REMOVE_FILE
#define LANDLOCK_ACCESS_FS_REMOVE_FILE (1ULL << 5)
#endif
#ifndef LANDLOCK_ACCESS_FS_MAKE_CHAR
#define LANDLOCK_ACCESS_FS_MAKE_CHAR (1ULL << 6)
#endif
#ifndef LANDLOCK_ACCESS_FS_MAKE_DIR
#define LANDLOCK_ACCESS_FS_MAKE_DIR (1ULL << 7)
#endif
#ifndef LANDLOCK_ACCESS_FS_MAKE_REG
#define LANDLOCK_ACCESS_FS_MAKE_REG (1ULL << 8)
#endif
#ifndef LANDLOCK_ACCESS_FS_MAKE_SOCK
#define LANDLOCK_ACCESS_FS_MAKE_SOCK (1ULL << 9)
#endif
#ifndef LANDLOCK_ACCESS_FS_MAKE_FIFO
#define LANDLOCK_ACCESS_FS_MAKE_FIFO (1ULL << 10)
#endif
#ifndef LANDLOCK_ACCESS_FS_MAKE_BLOCK
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK (1ULL << 11)
#endif
#ifndef LANDLOCK_ACCESS_FS_MAKE_SYM
#define LANDLOCK_ACCESS_FS_MAKE_SYM (1ULL << 12)
#endif
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)  // ABI 2 (Linux 5.19)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)  // ABI 3 (Linux 6.2)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)  // ABI 5 (Linux 6.10)
#endif

// landlock_restrict_self() audit-logging flags, ABI 7 (Linux 6.15). With audit
// enabled (CONFIG_AUDIT + auditd), the kernel emits LANDLOCK_ACCESS records for
// denied operations. By default only denials in the enforcing process itself
// ("same exec") are logged; LOG_NEW_EXEC_ON extends logging to descendants
// after execve() so denials in forked/exec'd children are auditable too.
#ifndef LANDLOCK_RESTRICT_SELF_LOG_SAME_EXEC_OFF
#define LANDLOCK_RESTRICT_SELF_LOG_SAME_EXEC_OFF (1U << 0)
#endif
#ifndef LANDLOCK_RESTRICT_SELF_LOG_NEW_EXEC_ON
#define LANDLOCK_RESTRICT_SELF_LOG_NEW_EXEC_ON (1U << 1)
#endif
#ifndef LANDLOCK_RESTRICT_SELF_LOG_SUBDOMAINS_OFF
#define LANDLOCK_RESTRICT_SELF_LOG_SUBDOMAINS_OFF (1U << 2)
#endif

namespace mongo {
namespace {

// Value of the kernel's `enum landlock_rule_type` for path-beneath rules. It
// is an enumerator, not a macro, in <linux/landlock.h>, so it gets a local
// name instead of a guarded #define (a macro with the uAPI name would break
// that header if it were ever included).
constexpr uint32_t kRulePathBeneath = 1;  // LANDLOCK_RULE_PATH_BENEATH

// Local mirrors of the uAPI structs, layout-compatible with the kernel's
// definitions (see the static_asserts). LandlockRulesetAttr deliberately
// carries only the filesystem member: the struct size is passed to
// landlock_create_ruleset() explicitly, and the kernel zero-fills every field
// it knows about beyond that size (handled_access_net, scoped), so this
// FS-only prefix is valid on every ABI version and handles no network or
// scope restrictions.
struct LandlockRulesetAttr {
    uint64_t handledAccessFs = 0;
};
static_assert(sizeof(LandlockRulesetAttr) == 8,
              "must match the handled_access_fs prefix of struct landlock_ruleset_attr");

struct LandlockPathBeneathAttr {
    uint64_t allowedAccess = 0;
    int32_t parentFd = -1;
} __attribute__((packed));
static_assert(sizeof(LandlockPathBeneathAttr) == 12,
              "must match struct landlock_path_beneath_attr");

// Thin syscall(2) shims: glibc only grew wrappers for the Landlock syscalls in
// 2.41, so they are invoked directly.
long landlockCreateRuleset(const LandlockRulesetAttr* attr, size_t size, uint32_t flags) {
    return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}

long landlockAddRule(int rulesetFd, uint32_t ruleType, const void* ruleAttr, uint32_t flags) {
    return syscall(__NR_landlock_add_rule, rulesetFd, ruleType, ruleAttr, flags);
}

long landlockRestrictSelf(int rulesetFd, uint32_t flags) {
    return syscall(__NR_landlock_restrict_self, rulesetFd, flags);
}

// The running kernel's Landlock ABI version, via landlock_create_ruleset(2)'s
// LANDLOCK_CREATE_RULESET_VERSION query -- the documented feature-detection
// call; nothing is created. Fails when the kernel lacks Landlock (pre-5.13)
// or has it disabled in its LSM stack.
StatusWith<long> landlockAbiVersion() {
    const long abi = landlockCreateRuleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi >= 0) {
        return abi;
    }
    const auto ec = lastSystemError();
    if (ec == std::errc::function_not_supported) {  // ENOSYS
        return Status(ErrorCodes::OperationFailed,
                      "Landlock is not supported by the running kernel");
    }
    if (ec == std::errc::operation_not_supported) {  // EOPNOTSUPP
        return Status(ErrorCodes::OperationFailed,
                      "Landlock is supported by the running kernel but disabled in its LSM stack");
    }
    return Status(ErrorCodes::OperationFailed,
                  str::stream() << "Failed to probe Landlock ABI: " << errorMessage(ec));
}

constexpr uint64_t kAccessRead = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

constexpr uint64_t kAccessMutate = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
    LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
    LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
    LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER |
    LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_IOCTL_DEV;

constexpr uint64_t kAccessReadWrite = kAccessRead | kAccessMutate;

// Everything the ruleset handles (denies by default), including EXECUTE.
// EXECUTE is handled but deliberately never grantable (see addPathRule):
// mongod and mongos never execute(2) files, so under the sandbox nothing may --
// note Landlock's EXECUTE only gates execve()-style execution, not mmap'ing
// libraries, so dlopen() needs only READ_FILE and still works.
constexpr uint64_t kAccessFsAll = kAccessRead | kAccessMutate | LANDLOCK_ACCESS_FS_EXECUTE;

// Rights that make sense on a non-directory. The kernel rejects (EINVAL) a
// path-beneath rule granting directory-shaped rights on a file, so rules for
// files are intersected with this mask.
constexpr uint64_t kAccessFileCompatible = LANDLOCK_ACCESS_FS_WRITE_FILE |
    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_IOCTL_DEV;

// Filesystem rights the probed kernel ABI knows about. Handling a right the
// kernel does not know about makes landlock_create_ruleset() fail, so
// requested rights are intersected with this; a dropped bit simply means that
// class of access is not policed on this kernel (best-effort).
uint64_t supportedFsAccess(long abi) {
    // ABI 1 (Linux 5.13): EXECUTE through MAKE_SYM.
    uint64_t supported = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) {
        supported |= LANDLOCK_ACCESS_FS_REFER;
    }
    if (abi >= 3) {
        supported |= LANDLOCK_ACCESS_FS_TRUNCATE;
    }
    if (abi >= 5) {
        supported |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    }
    return supported;
}

// The uAPI names of the filesystem rights in `mask`, for structured logs that
// are directly cross-referenceable against strace output and
// <linux/landlock.h>.
std::vector<std::string_view> fsAccessRightNames(uint64_t mask) {
    static constexpr std::pair<uint64_t, std::string_view> kFsRightNames[] = {
        {LANDLOCK_ACCESS_FS_EXECUTE, "LANDLOCK_ACCESS_FS_EXECUTE"},
        {LANDLOCK_ACCESS_FS_WRITE_FILE, "LANDLOCK_ACCESS_FS_WRITE_FILE"},
        {LANDLOCK_ACCESS_FS_READ_FILE, "LANDLOCK_ACCESS_FS_READ_FILE"},
        {LANDLOCK_ACCESS_FS_READ_DIR, "LANDLOCK_ACCESS_FS_READ_DIR"},
        {LANDLOCK_ACCESS_FS_REMOVE_DIR, "LANDLOCK_ACCESS_FS_REMOVE_DIR"},
        {LANDLOCK_ACCESS_FS_REMOVE_FILE, "LANDLOCK_ACCESS_FS_REMOVE_FILE"},
        {LANDLOCK_ACCESS_FS_MAKE_CHAR, "LANDLOCK_ACCESS_FS_MAKE_CHAR"},
        {LANDLOCK_ACCESS_FS_MAKE_DIR, "LANDLOCK_ACCESS_FS_MAKE_DIR"},
        {LANDLOCK_ACCESS_FS_MAKE_REG, "LANDLOCK_ACCESS_FS_MAKE_REG"},
        {LANDLOCK_ACCESS_FS_MAKE_SOCK, "LANDLOCK_ACCESS_FS_MAKE_SOCK"},
        {LANDLOCK_ACCESS_FS_MAKE_FIFO, "LANDLOCK_ACCESS_FS_MAKE_FIFO"},
        {LANDLOCK_ACCESS_FS_MAKE_BLOCK, "LANDLOCK_ACCESS_FS_MAKE_BLOCK"},
        {LANDLOCK_ACCESS_FS_MAKE_SYM, "LANDLOCK_ACCESS_FS_MAKE_SYM"},
        {LANDLOCK_ACCESS_FS_REFER, "LANDLOCK_ACCESS_FS_REFER"},
        {LANDLOCK_ACCESS_FS_TRUNCATE, "LANDLOCK_ACCESS_FS_TRUNCATE"},
        {LANDLOCK_ACCESS_FS_IOCTL_DEV, "LANDLOCK_ACCESS_FS_IOCTL_DEV"},
    };

    std::vector<std::string_view> names;
    for (auto&& [bit, name] : kFsRightNames) {
        if (mask & bit) {
            names.push_back(name);
        }
    }
    return names;
}

// Back the "landlock" serverStatus section (see LandlockServerStatusSection):
// whether the sandbox option is enabled, the Landlock ABI version probed from
// the running kernel (0 when the kernel lacks Landlock or has it disabled),
// both reported even when the sandbox option is off, whether the sandbox is
// actually enforced ("active"), and the filesystem access-right masks the
// enforced ruleset handles and had to degrade (meaningful only while active).
//
// Deliberately not atomic: written once, from the single-threaded
// EnableLandlockSandbox startup initializer, before any thread that could read
// them (command threads serving serverStatus) is spawned -- thread creation
// establishes the necessary happens-before. Must become atomic if enforcement
// ever moves out of single-threaded startup.
bool gLandlockEnabled = false;
int gLandlockAbi = 0;
bool gLandlockActive = false;
uint64_t gLandlockHandledFsAccess = 0;
uint64_t gLandlockDegradedFsAccess = 0;

}  // namespace

/**
 * A single filesystem grant: every filesystem object beneath `path` may be
 * accessed with the rights in `accessMask`. Rules are inert descriptions; they
 * take effect when passed to LandlockRuleset::addPathRule().
 *
 * Only constructible through the named factories, so every grantable access
 * profile is a vetted constant rather than an ad-hoc bit mask. Rules carry no
 * ABI knowledge (they may be built before any ruleset exists); rights the
 * running kernel does not support are dropped when the rule is added to a
 * ruleset, which is the only place that can know the ABI.
 */
class LandlockFilesystemRule {
public:
    /** Read files and list directories beneath `path`. */
    static LandlockFilesystemRule readOnly(std::string path) {
        return {std::move(path), kAccessRead};
    }

    /** Full read and mutate rights, for data hierarchies. */
    static LandlockFilesystemRule readWrite(std::string path) {
        return {std::move(path), kAccessReadWrite};
    }

    const std::string& path() const {
        return _path;
    }

    uint64_t accessMask() const {
        return _accessMask;
    }

private:
    LandlockFilesystemRule(std::string path, uint64_t accessMask)
        : _path(std::move(path)), _accessMask(accessMask) {}

    std::string _path;
    uint64_t _accessMask;
};

/**
 * Owns one Landlock ruleset file descriptor and wraps the three Landlock
 * syscalls: landlock_create_ruleset() (in create()), landlock_add_rule() (in
 * addPathRule()) and landlock_restrict_self() (in restrictSelf()).
 *
 * Filesystem-only for now: the ruleset handles no network or scope
 * restrictions (see LandlockRulesetAttr).
 *
 * ABI resolution happens here: create() probes the running kernel's Landlock
 * ABI version and the ruleset handles the intersection of the requested rights
 * and what that ABI supports; rules are likewise masked down when added.
 *
 * Neither copyable nor movable, so exactly one owner of the ruleset fd can
 * exist. Intended use is a single instance during startup: create(), add every
 * rule, then call restrictSelf() once -- after which the policy is permanent
 * for the process and its descendants. Calling addPathRule() after
 * restrictSelf() is a programming error and process-fatal: the kernel takes a
 * snapshot of the ruleset at enforcement time, so a late rule would appear to
 * succeed while silently never applying to this process.
 */
class LandlockRuleset {
    // Passkey idiom: construction goes through create() only, but the
    // constructor must be public so std::make_unique can call it.
    struct Passkey {
        explicit Passkey() = default;
    };

public:
    LandlockRuleset(
        Passkey, int rulesetFd, int abi, uint64_t requestedFsAccess, uint64_t handledFsAccess)
        : _rulesetFd(rulesetFd),
          _abi(abi),
          _requestedFsAccess(requestedFsAccess),
          _handledFsAccess(handledFsAccess) {}

    /**
     * Probes the Landlock ABI and creates a ruleset handling `requestedFsAccess`
     * masked to the probed ABI. Fails with a descriptive Status when the kernel
     * lacks or has disabled Landlock.
     */
    static StatusWith<std::unique_ptr<LandlockRuleset>> create(
        uint64_t requestedFsAccess = kAccessFsAll) {
        uassert(13118813,
                "A Landlock ruleset must be asked to handle at least one filesystem access right",
                requestedFsAccess != 0);

        auto swAbi = landlockAbiVersion();
        if (!swAbi.isOK()) {
            return swAbi.getStatus();
        }
        const long abi = swAbi.getValue();

        LandlockRulesetAttr attr;
        // Every rule added later is intersected with this handled set (see
        // addPathRule): the kernel rejects (EINVAL) a rule granting a right the
        // ruleset does not handle, e.g. IOCTL_DEV in a rule when this mask
        // dropped it because the running ABI predates it.
        attr.handledAccessFs = requestedFsAccess & supportedFsAccess(abi);
        if (attr.handledAccessFs == 0) {
            LOGV2_WARNING(13118801,
                          "Landlock: none of the requested access rights are supported by the "
                          "running kernel's ABI",
                          "abiVersion"_attr = abi,
                          "requestedRights"_attr = fsAccessRightNames(requestedFsAccess));
            return Status(
                ErrorCodes::InvalidOptions,
                "None of the requested Landlock filesystem access rights are supported by this "
                "kernel");
        }
        // Feature detection: rights we want to handle that this kernel's ABI
        // does not know about are dropped from the ruleset and therefore stay
        // unrestricted.
        if (const uint64_t degraded = requestedFsAccess & ~attr.handledAccessFs; degraded != 0) {
            LOGV2(13118800,
                  "Landlock: some requested access rights are not supported by the running "
                  "kernel's ABI and will not be restricted",
                  "abiVersion"_attr = abi,
                  "degradedRights"_attr = fsAccessRightNames(degraded));
        }
        LOGV2(13118802,
              "Landlock: access rights the ruleset will handle (denied by default)",
              "abiVersion"_attr = abi,
              "handledRights"_attr = fsAccessRightNames(attr.handledAccessFs));

        const int rulesetFd = static_cast<int>(landlockCreateRuleset(&attr, sizeof(attr), 0));
        if (rulesetFd < 0) {
            LOGV2_FATAL(13118803,
                        "Failed to create Landlock ruleset",
                        "error"_attr = errorMessage(lastSystemError()));
        }
        return std::make_unique<LandlockRuleset>(
            Passkey{}, rulesetFd, static_cast<int>(abi), requestedFsAccess, attr.handledAccessFs);
    }

    ~LandlockRuleset() {
        close(_rulesetFd);
    }

    LandlockRuleset(const LandlockRuleset&) = delete;
    LandlockRuleset& operator=(const LandlockRuleset&) = delete;
    LandlockRuleset(LandlockRuleset&&) = delete;
    LandlockRuleset& operator=(LandlockRuleset&&) = delete;

    int abiVersion() const {
        return _abi;
    }

    /** The rights this ruleset denies by default (requested masked to the ABI). */
    uint64_t handledFsAccess() const {
        return _handledFsAccess;
    }

    /** Requested rights the running kernel's ABI cannot restrict. */
    uint64_t degradedFsAccess() const {
        return _requestedFsAccess & ~_handledFsAccess;
    }

    /**
     * Grants the rule's access mask (intersected with the rights this ruleset
     * handles, and with file-compatible rights when the path is not a
     * directory) beneath the rule's path.
     *
     * A path that does not exist returns OK without adding anything: the
     * policy lists every path the server could need, and hierarchies absent on
     * this system (or files not created yet, which must instead be covered by
     * a rule on their parent directory) simply get no grant.
     *
     * Must not be called once restrictSelf() has been called (process-fatal).
     */
    Status addPathRule(const LandlockFilesystemRule& rule) {
        invariant(!_restricted,
                  str::stream() << "Attempted to add a Landlock rule for '" << rule.path()
                                << "' after restrictSelf(); rules added after enforcement "
                                   "silently never apply");

        // O_PATH: a location handle is all a rule needs; no read access to the
        // object itself is required.
        const int pathFd = open(rule.path().c_str(), O_PATH | O_CLOEXEC);
        if (pathFd < 0) {
            const auto ec = lastSystemError();
            if (ec == std::errc::no_such_file_or_directory) {
                LOGV2(13118804,
                      "Landlock: skipping rule for nonexistent path",
                      "path"_attr = rule.path());
                return Status::OK();
            }
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to open '" << rule.path()
                                        << "' for a Landlock rule: " << errorMessage(ec));
        }
        ScopeGuard closePathFd([&] { close(pathFd); });

        struct stat statbuf;
        if (fstat(pathFd, &statbuf) != 0) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream()
                              << "Failed to stat '" << rule.path()
                              << "' for a Landlock rule: " << errorMessage(lastSystemError()));
        }

        LandlockPathBeneathAttr attr;
        // EXECUTE is stripped from every grant: the server never executes files,
        // so no rule may re-allow execution anywhere (it stays denied by default,
        // since the ruleset handles it -- see kAccessFsAll).
        attr.allowedAccess = rule.accessMask() & _handledFsAccess & ~LANDLOCK_ACCESS_FS_EXECUTE;
        if (!S_ISDIR(statbuf.st_mode)) {
            attr.allowedAccess &= kAccessFileCompatible;
        }
        // An empty access set is rejected by the kernel (ENOMSG); it means every
        // requested right was masked out above, so there is nothing to grant.
        if (attr.allowedAccess == 0) {
            return Status::OK();
        }
        attr.parentFd = pathFd;

        if (landlockAddRule(_rulesetFd, kRulePathBeneath, &attr, 0) != 0) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to add Landlock rule for '" << rule.path()
                                        << "': " << errorMessage(lastSystemError()));
        }

        LOGV2(13118805,
              "Landlock filepath rule applied",
              "ruleType"_attr = "path_beneath",
              "path"_attr = rule.path(),
              "allowedAccess"_attr = fsAccessRightNames(attr.allowedAccess));
        return Status::OK();
    }

    /**
     * The point of no return: enforces the ruleset on the current process,
     * permanently and inherited across fork()/execve(). Call once, after every
     * rule has been added; from this call on, addPathRule() is forbidden.
     */
    Status restrictSelf() {
        // Finalize the ruleset even if enforcement fails below: addPathRule()
        // treats any call after this one as a programming error.
        _restricted = true;

        // Required so an unprivileged process may restrict itself.
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to set PR_SET_NO_NEW_PRIVS: "
                                        << errorMessage(lastSystemError()));
        }

        // Ask the kernel to audit-log denials in exec'd descendants too. On
        // ABI < 7 any nonzero flag fails with EINVAL, so it is only passed when
        // supported.
        const uint32_t flags = _abi >= 7 ? LANDLOCK_RESTRICT_SELF_LOG_NEW_EXEC_ON : 0;
        if (landlockRestrictSelf(_rulesetFd, flags) != 0) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to enforce Landlock ruleset: "
                                        << errorMessage(lastSystemError()));
        }
        return Status::OK();
    }

private:
    const int _rulesetFd;
    const int _abi;
    const uint64_t _requestedFsAccess;
    const uint64_t _handledFsAccess;
    bool _restricted = false;
};

namespace {

/**
 * Builds the server's ruleset and enforces it. Best-effort by design: on any
 * failure this logs a warning and leaves the process unsandboxed rather than
 * enforcing an incomplete policy that would break the server.
 */
void initializeLandlock() {
    auto swRuleset = LandlockRuleset::create();
    if (!swRuleset.isOK()) {
        LOGV2_WARNING(13118806,
                      "Failed to create Landlock ruleset; continuing without filesystem "
                      "sandboxing",
                      "error"_attr = swRuleset.getStatus());
        return;
    }
    auto& ruleset = *swRuleset.getValue();

    // TODO: SERVER-130423 Populate the filesystem path policy: every hierarchy
    // the server needs (data directory read-write, log/pid file parent
    // directories, system libraries, /proc, ...), which is still under
    // investigation. Grants are described with the LandlockFilesystemRule
    // factories, e.g.
    //     rules.push_back(LandlockFilesystemRule::readWrite(storageGlobalParams.dbpath));
    std::vector<LandlockFilesystemRule> rules;

    // Enforcing with no rules would deny every filesystem access and the server
    // could not run, so the sandbox stays disengaged until a policy is defined.
    if (rules.empty()) {
        LOGV2(13118807,
              "Landlock filesystem path policy is not defined yet; continuing without "
              "filesystem sandboxing",
              "abiVersion"_attr = ruleset.abiVersion());
        return;
    }

    LOGV2(13118808,
          "Applying Landlock filesystem sandbox",
          "abiVersion"_attr = ruleset.abiVersion(),
          "ruleCount"_attr = rules.size());

    for (const auto& rule : rules) {
        if (Status status = ruleset.addPathRule(rule); !status.isOK()) {
            LOGV2_WARNING(13118809,
                          "Failed to add Landlock rule; continuing without filesystem sandboxing",
                          "path"_attr = rule.path(),
                          "error"_attr = status);
            return;
        }
    }

    if (ruleset.abiVersion() < 7) {
        LOGV2(13118810,
              "Landlock ABI does not support audit logging of denials (requires ABI 7, Linux "
              "6.15+); denied operations will not appear in the kernel audit log",
              "abiVersion"_attr = ruleset.abiVersion());
    }

    if (Status status = ruleset.restrictSelf(); !status.isOK()) {
        LOGV2_WARNING(13118811,
                      "Failed to enforce Landlock ruleset; continuing without filesystem "
                      "sandboxing",
                      "error"_attr = status);
        return;
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
    handledRights.append("fs", fsAccessRightNames(ruleset.handledFsAccess()));
    handledRights.append("net", std::vector<std::string>{});
    handledRights.append("scope", std::vector<std::string>{});
    LOGV2(13118812,
          "Landlock ruleset applied",
          "abiVersion"_attr = ruleset.abiVersion(),
          "handledAccessRights"_attr = handledRights.obj(),
          "degradedRights"_attr = fsAccessRightNames(ruleset.degradedFsAccess()));
}

// Applies the Landlock sandbox if the operator enabled it via --landlock /
// security.landlock.enabled (default: disabled while this is a POC).
//
// Runs after the known file-touching initializers (ServerLogRedirection opens
// the log and backtrace files; ForkServer reopens the standard streams and
// chdirs) and before the "default" group. Everything gated on "default" is
// guaranteed to run sandboxed. Other pre-"default" initializers stay unordered
// against this one on purpose: test builds shuffle them, so an unlisted
// file-toucher fails loudly in testing and gets added as a prerequisite here.
MONGO_INITIALIZER_GENERAL(EnableLandlockSandbox,
                          ("EndStartupOptionHandling", "ForkServer", "ServerLogRedirection"),
                          ("default"))
(InitializerContext*) {
    const auto& params = optionenvironment::startupOptionsParsed;
    bool enabled = false;
    if (params.count("security.landlock.enabled")) {
        enabled = params["security.landlock.enabled"].as<bool>();
    }
    gLandlockEnabled = enabled;
    const auto swAbi = landlockAbiVersion();
    gLandlockAbi = swAbi.isOK() ? static_cast<int>(swAbi.getValue()) : 0;
    if (!enabled) {
        LOGV2(13118814, "Skipping Landlock initialization: sandboxing is not enabled");
        return;
    }

    initializeLandlock();
}

// Read-only diagnostic for monitoring and tests:
//
//   enabled:              the security.landlock.enabled option
//   active:               whether the sandbox is actually enforced
//   abiVersion:           Landlock ABI probed from the running kernel (0 when
//                         the kernel lacks Landlock or has it disabled),
//                         reported even when the sandbox option is off
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
        builder.append("enabled", gLandlockEnabled);
        builder.append("active", gLandlockActive);
        builder.append("abiVersion", gLandlockAbi);
        if (gLandlockActive) {
            {
                BSONObjBuilder handled(builder.subobjStart("handledAccessRights"));
                handled.append("fs", fsAccessRightNames(gLandlockHandledFsAccess));
            }
            {
                BSONObjBuilder degraded(builder.subobjStart("degradedAccessRights"));
                degraded.append("fs", fsAccessRightNames(gLandlockDegradedFsAccess));
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
