// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// The Landlock sandbox implementation (Linux only).
//
// The Landlock uAPI (struct layouts, access-right bits, syscall numbers) is
// defined locally in this file rather than by including <linux/landlock.h>:
// toolchain sysroot headers routinely lag the kernel we actually run on, and
// the uAPI is append-only and stable by contract, so mirroring it is safe.
// Every constant is guarded by #ifndef so a definition supplied by some other
// header always wins. The running kernel's Landlock ABI version, probed by
// LandlockRuleset, decides which of these rights are actually enforced.

#if defined(__linux__)

#include "mongo/util/landlock/landlock.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>

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

constexpr uint64_t kAccessRead = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

constexpr uint64_t kAccessMutate = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
    LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
    LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
    LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;

constexpr uint64_t kAccessReadWrite = kAccessRead | kAccessMutate;

// Everything the ruleset handles (denies by default). Deliberately broader
// than any grantable mask, so three rights stay denied everywhere for the
// life of the process:
//  - EXECUTE: mongod and mongos never execve(2) files (config-file __exec
//    expansion runs during option parsing, before the sandbox engages). Note
//    Landlock's EXECUTE only gates execve()-style execution, not mmap'ing
//    libraries, so dlopen() needs only READ_FILE and still works.
//  - REFER: the server never renames or links files across directories; the
//    renames it does perform (log rotation, WiredTiger's turtle file, FTDC's
//    interim file) all stay within one directory, which REFER does not gate.
//  - IOCTL_DEV: the server needs no device ioctls; the ioctls it does issue
//    are on sockets and pipes, which are not filesystem device files.
constexpr uint64_t kAccessFsAll = kAccessReadWrite | LANDLOCK_ACCESS_FS_EXECUTE |
    LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_IOCTL_DEV;

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

}  // namespace

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

LandlockFilesystemRule LandlockFilesystemRule::readOnly(std::string path) {
    return {std::move(path), kAccessRead};
}

LandlockFilesystemRule LandlockFilesystemRule::readWrite(std::string path) {
    return {std::move(path), kAccessReadWrite};
}

StatusWith<LandlockFilesystemRule> LandlockFilesystemRule::fromConfigString(
    std::string_view entry) {
    // Every diagnostic quotes the entry exactly as written. Stray whitespace,
    // an empty entry, etc. is visible
    const auto malformed = [&](const std::string& detail) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Invalid security.landlock.additionalPathRules entry '"
                                    << entry << "': " << detail
                                    << ". Expected \"r:<absolute path>\" or "
                                       "\"rw:<absolute path>\"");
    };

    // The first colon only: the access specifier cannot contain one, but a path can.
    const auto separator = entry.find(':');
    if (separator == std::string_view::npos) {
        return malformed("missing the '<access>:' prefix");
    }
    const auto access = entry.substr(0, separator);
    const auto path = entry.substr(separator + 1);

    if (access.empty()) {
        return malformed("no access specifier precedes the ':'");
    }
    if (path.empty()) {
        return malformed("no path follows the access specifier");
    }
    // A relative path would be resolved against whatever directory the process happens
    // to be in -- "/" once --fork has chdir'd
    if (!path.starts_with('/')) {
        return malformed("the path must be absolute");
    }

    if (access == "r") {
        return readOnly(std::string{path});
    }
    if (access == "rw") {
        return readWrite(std::string{path});
    }
    return malformed(str::stream() << "unknown access specifier '" << access << "'");
}

std::vector<std::string_view> LandlockRuleset::fsAccessRightNames(uint64_t mask) {
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

LandlockRuleset::~LandlockRuleset() {
    close(_rulesetFd);
}

StatusWith<std::unique_ptr<LandlockRuleset>> LandlockRuleset::create() {
    uint64_t requestedFsAccess = kAccessFsAll;
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

Status LandlockRuleset::addPathRule(const LandlockFilesystemRule& rule) {
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

Status LandlockRuleset::restrictSelf() {
    invariant(!_restricted,
              "Attempted to call restrictSelf() twice; the Landlock policy is already "
              "permanent for this process");

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

Status validateLandlockAdditionalPathRules(const std::vector<std::string>& entries) {
    for (const auto& entry : entries) {
        if (auto swRule = LandlockFilesystemRule::fromConfigString(entry); !swRule.isOK()) {
            return swRule.getStatus();
        }
    }
    return Status::OK();
}

}  // namespace mongo


#endif  // defined(__linux__)
