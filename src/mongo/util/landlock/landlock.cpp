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
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>

#include <boost/filesystem/operations.hpp>
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

// Installed by ScopedLandlockAddRuleHook; null outside of tests.
const LandlockAddRuleHook* gAddRuleHook = nullptr;

// Rights the server's own rules never need, so only an operator's rule may grant them.
constexpr uint64_t kAccessOperatorOnly = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_IOCTL_DEV;

constexpr uint64_t kAccessRead = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

constexpr uint64_t kAccessMutate = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
    LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
    LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
    LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;

constexpr uint64_t kAccessReadWrite = kAccessRead | kAccessMutate;

// Rights that make sense on a non-directory. The kernel rejects (EINVAL) a
// path-beneath rule granting directory-shaped rights on a file, so rules for
// files are intersected with this mask.
constexpr uint64_t kAccessFileCompatible = LANDLOCK_ACCESS_FS_WRITE_FILE |
    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_EXECUTE |
    LANDLOCK_ACCESS_FS_IOCTL_DEV;

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

// Every filesystem right, by bit, with its uAPI name and the name an additionalPathRules
// entry spells it with (the uAPI name without the LANDLOCK_ACCESS_FS_ prefix).
struct FsRightName {
    uint64_t bit;
    std::string_view configName;
    std::string_view uapiName;
};
constexpr FsRightName kFsRights[] = {
#define MONGO_FS_RIGHT(NAME) {LANDLOCK_ACCESS_FS_##NAME, #NAME, "LANDLOCK_ACCESS_FS_" #NAME}
    MONGO_FS_RIGHT(EXECUTE),
    MONGO_FS_RIGHT(WRITE_FILE),
    MONGO_FS_RIGHT(READ_FILE),
    MONGO_FS_RIGHT(READ_DIR),
    MONGO_FS_RIGHT(REMOVE_DIR),
    MONGO_FS_RIGHT(REMOVE_FILE),
    MONGO_FS_RIGHT(MAKE_CHAR),
    MONGO_FS_RIGHT(MAKE_DIR),
    MONGO_FS_RIGHT(MAKE_REG),
    MONGO_FS_RIGHT(MAKE_SOCK),
    MONGO_FS_RIGHT(MAKE_FIFO),
    MONGO_FS_RIGHT(MAKE_BLOCK),
    MONGO_FS_RIGHT(MAKE_SYM),
    MONGO_FS_RIGHT(REFER),
    MONGO_FS_RIGHT(TRUNCATE),
    MONGO_FS_RIGHT(IOCTL_DEV),
#undef MONGO_FS_RIGHT
};

// The policy table. Released entries are immutable: to handle more, append a version that adds
// rights; never edit or reorder an existing one.
// landlock_test.cpp pins the released values and checks the monotonicity invariants.
constexpr LandlockPolicy kPolicies[] = {
    // v1: every right the sandbox handled when it first shipped.
    {1,
     kAccessReadWrite | LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_REFER |
         LANDLOCK_ACCESS_FS_IOCTL_DEV},
};

}  // namespace

std::span<const LandlockPolicy> LandlockPolicy::all() {
    return kPolicies;
}

const LandlockPolicy& LandlockPolicy::latest() {
    return kPolicies[std::size(kPolicies) - 1];
}

StatusWith<const LandlockPolicy*> LandlockPolicy::forVersion(long long version) {
    if (version < 1 || version > latest().version()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "security.landlock.policyVersion is " << version
                                    << ", but this server implements Landlock policy versions 1 "
                                       "through "
                                    << latest().version());
    }
    return &kPolicies[version - 1];
}

Status validateLandlockPolicyVersion(const int& version) {
    return LandlockPolicy::forVersion(version).getStatus();
}

boost::filesystem::path canonicalizePathForLandlock(const std::string& path,
                                                    const boost::filesystem::path& base) {
    if (path.empty()) {
        return {};
    }

    const auto absolutePath = boost::filesystem::absolute(path, base);
    boost::system::error_code ec;
    auto resolved = boost::filesystem::weakly_canonical(absolutePath, ec);

    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Failed to resolve the path '" << path << "' (as '"
                          << absolutePath.string() << "') for a Landlock rule: " << ec.message(),
            !ec);

    // For a path that exists, weakly_canonical() resolves a trailing dot element away; for
    // one that does not, it survives into the result. See the header for why it must not.
    if (resolved.filename_is_dot()) {
        resolved = resolved.parent_path();
    }
    return resolved;
}

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

LandlockFilesystemRule LandlockFilesystemRule::readOnly(std::string path,
                                                        boost::optional<uint64_t> extraRights) {
    return {std::move(path), kAccessRead | extraRights.value_or(0)};
}

LandlockFilesystemRule LandlockFilesystemRule::readWrite(std::string path,
                                                         boost::optional<uint64_t> extraRights) {
    return {std::move(path), kAccessReadWrite | extraRights.value_or(0)};
}

StatusWith<LandlockFilesystemRule> LandlockFilesystemRule::fromConfigString(
    std::string_view entry) {
    // Every diagnostic quotes the entry exactly as written. Stray whitespace,
    // an empty entry, etc. is visible
    const auto malformed = [&](const std::string& detail) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Invalid security.landlock.additionalPathRules entry '"
                                    << entry << "': " << detail
                                    << ". Expected \"<access>[,<access>...]:<absolute path>\", "
                                       "where each <access> is r, rw, or a "
                                       "LANDLOCK_ACCESS_FS_* name without the prefix");
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

    uint64_t mask = 0;
    std::string_view remaining = access;
    while (true) {
        const auto comma = remaining.find(',');
        const auto token = remaining.substr(0, comma);
        if (token.empty()) {
            return malformed("empty access specifier in the comma-separated list");
        }

        if (str::equalCaseInsensitive(token, "r")) {
            mask |= kAccessRead;
        } else if (str::equalCaseInsensitive(token, "rw")) {
            mask |= kAccessReadWrite;
        } else {
            const auto right = std::find_if(
                std::begin(kFsRights), std::end(kFsRights), [&](const FsRightName& candidate) {
                    return str::equalCaseInsensitive(token, candidate.configName);
                });
            if (right == std::end(kFsRights)) {
                return malformed(str::stream() << "unknown access specifier '" << token << "'");
            }
            mask |= right->bit;
        }

        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1);
    }
    return LandlockFilesystemRule{std::string{path}, mask};
}

std::vector<std::string_view> LandlockRuleset::fsAccessRightNames(uint64_t mask) {
    std::vector<std::string_view> names;
    for (const auto& right : kFsRights) {
        if (mask & right.bit) {
            names.push_back(right.uapiName);
        }
    }
    return names;
}

LandlockRuleset::~LandlockRuleset() {
    close(_rulesetFd);
    for (const int fd : _pinnedPathFds) {
        close(fd);
    }
}

StatusWith<std::unique_ptr<LandlockRuleset>> LandlockRuleset::create(const LandlockPolicy& policy) {
    uint64_t requestedFsAccess = policy.handledFsAccess();
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
          "policyVersion"_attr = policy.version(),
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

ScopedLandlockAddRuleHook::ScopedLandlockAddRuleHook(LandlockAddRuleHook hook)
    : _hook(std::move(hook)) {
    // The hook skips the kernel, so it must never be reachable in a production process.
    // Unit tests run with testing diagnostics on; a server does only when started with
    // --setParameter testingDiagnosticsEnabled=true, which only test infrastructure does.
    invariant(
        TestingProctor::instance().isInitialized() && TestingProctor::instance().isEnabled(),
        "a Landlock add-rule hook may only be installed when testing diagnostics are enabled");
    invariant(!gAddRuleHook, "a Landlock add-rule hook is already installed");
    gAddRuleHook = &_hook;
}

ScopedLandlockAddRuleHook::~ScopedLandlockAddRuleHook() {
    gAddRuleHook = nullptr;
}

Status LandlockRuleset::addPathRule(const LandlockFilesystemRule& rule, bool internal) {
    invariant(!_restricted,
              str::stream() << "Attempted to add a Landlock rule for '" << rule.path()
                            << "' after restrictSelf(); rules added after enforcement "
                               "silently never apply");
    invariant(!internal || (rule.accessMask() & kAccessOperatorOnly) == 0,
              str::stream() << "Internal Landlock rule for '" << rule.path()
                            << "' requests EXECUTE or IOCTL_DEV, which only an operator's "
                               "additionalPathRules entry may grant");

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
    // Internal rules were checked above to carry no EXECUTE or IOCTL_DEV; an operator's
    // additionalPathRules entry can ask for them, and then it is theirs to grant.
    attr.allowedAccess = rule.accessMask() & _handledFsAccess;
    if (!S_ISDIR(statbuf.st_mode)) {
        attr.allowedAccess &= kAccessFileCompatible;
    }
    // An empty access set is rejected by the kernel (ENOMSG); it means every
    // requested right was masked out above, so there is nothing to grant.
    if (attr.allowedAccess == 0) {
        return Status::OK();
    }
    attr.parentFd = pathFd;

    const long addRuleResult = MONGO_unlikely(gAddRuleHook != nullptr)
        ? (*gAddRuleHook)(_rulesetFd, attr.allowedAccess)
        : landlockAddRule(_rulesetFd, kRulePathBeneath, &attr, 0);
    if (addRuleResult != 0) {
        return Status(ErrorCodes::OperationFailed,
                      str::stream() << "Failed to add Landlock rule for '" << rule.path()
                                    << "': " << errorMessage(lastSystemError()));
    }

    // Hold the descriptor open for the life of this ruleset when the rule asked for it, so the
    // path keeps resolving to the inode the rule was built from rather than to a replacement.
    if (rule.pinsPath()) {
        // Recorded before dismissing the guard, so a throwing push_back still closes the fd.
        _pinnedPathFds.push_back(pathFd);
        closePathFd.dismiss();
    }

    LOGV2(13118805,
          "Landlock filepath rule applied",
          "ruleType"_attr = "path_beneath",
          "path"_attr = rule.path(),
          "ino"_attr = static_cast<long long>(statbuf.st_ino),
          "pinned"_attr = rule.pinsPath(),
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
