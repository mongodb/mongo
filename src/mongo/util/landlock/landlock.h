// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// Landlock self-sandboxing for mongod and mongos (Linux only).
//
// Landlock is a Linux LSM that lets an unprivileged process irreversibly drop
// its own ambient filesystem rights. When enabled via --landlockMode
// (security.landlock.mode), the server declares the set of path hierarchies it
// legitimately needs at startup (e.g. dbPath read-write, system libraries
// read-only) and the kernel denies every other filesystem access for the
// lifetime of the process and its descendants.
//
// Users describe what filesystem permissions they need by creating a
// LandlockFilesystemRule containing the path and the bit set of filesystem access rights
// (LANDLOCK_ACCESS_FS_*) that shall be granted that path, and adding it to
// the global LandlockRuleset.
#if defined(__linux__)

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/filesystem/path.hpp>

// Landlock filesystem access rights constants, mirrored from <linux/landlock.h>.
// #ifndef keeps these compatible with any header that may already define them.
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

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Resolves `path` the way Landlock will see it, in the two steps a caller must always
 * take together, and returns the path a rule should be built from.
 *
 * First absolute, against `base`: a configured path may be relative, and the process may
 * have chdir'd since it was written (the server does, when it forks), so the base has to
 * be supplied rather than read from the current directory. Then canonical, so symlinks,
 * "." and ".." are resolved -- a rule binds to the inode its path resolves to, so this is
 * the path the grant actually lands on. Trailing components need not exist.
 *
 * A trailing separator, which is how a directory is usually written in a config file
 * ("/var/log/ftdc/"), is carried by boost as a trailing dot element and is stripped here.
 * That matters most for a caller taking parent_path(): left in place, the dot element
 * shifts the parent down a level and the caller would name the wrong directory.
 *
 * An empty path resolves to an empty path, so a caller can pass an unset option straight
 * through -- the policy treats an empty path as a feature not in use. A path that does not
 * exist yet resolves too: its longest existing prefix is canonicalized and the remainder
 * normalized, which is what lets a policy name a file the server has still to create.
 *
 * Throws (OperationFailed) when the path cannot be resolved at all -- an ancestor that cannot
 * be searched, a symlink loop, a component that is not a directory. There is no best-effort
 * result in that case: the inode a rule would bind to is unknown, so any path this could
 * return instead risks granting a hierarchy nobody configured.
 */
boost::filesystem::path canonicalizePathForLandlock(const std::string& path,
                                                    const boost::filesystem::path& base);

/**
 * A versioned MongoDB Landlock policy: the set of Landlock access rights MongoDB chooses to
 * handle (deny by default) as of a given policy version. This is what a particular binary denies
 * by default before any best-effort ABI negotiation with the running kernel (see
 * LandlockRuleset::create).
 *
 * The policy version is a monotonically increasing integer, independent of the Landlock ABI
 * number: it versions the rights MongoDB chooses to handle, not the rights the kernel can
 * enforce. The two are allowed to diverge so MongoDB can lag behind the newest kernel ABI.
 *
 * Every released version is immutable, and a new version may only add rights, never remove any.
 * This lets security.landlock.policyVersion pin a configuration to the rights an older release
 * handled, so that upgrading MongoDB does not start denying access an existing
 * additionalPathRules entry never had to ask for. The invariants are enforced by
 * landlock_test.cpp.
 */
class LandlockPolicy {
public:
    constexpr LandlockPolicy(int version, uint64_t handledFsAccess)
        : _version(version), _handledFsAccess(handledFsAccess) {}

    /** This policy's version number, from 1 on. */
    constexpr int version() const {
        return _version;
    }

    /**
     * The Landlock access rights denied by default.
     */
    constexpr uint64_t handledFsAccess() const {
        return _handledFsAccess;
    }

    /** Every policy this binary implements, ordered by version: element i is version i + 1. */
    static std::span<const LandlockPolicy> all();

    /** The newest policy this binary implements, applied when no version is configured. */
    static const LandlockPolicy& latest();

    /**
     * The policy for security.landlock.policyVersion. InvalidOptions when `version` is below
     * 1 or newer than latest(): this binary cannot honor a policy it does not implement.
     */
    static StatusWith<const LandlockPolicy*> forVersion(long long version);

private:
    int _version;
    uint64_t _handledFsAccess;
};

/**
 * Checks a configured security.landlock.policyVersion, for the option's IDL validator, so a
 * version this binary does not implement is rejected at option parsing whatever the sandbox
 * mode is.
 */
Status validateLandlockPolicyVersion(const int& version);

/**
 * A single filesystem grant: every filesystem object beneath `path` may be
 * accessed with the rights in `accessMask`. Rules are inert descriptions; they
 * take effect when passed to LandlockRuleset::addPathRule().
 *
 * Rules carry no ABI knowledge (they may be built before any ruleset exists); rights the
 * running kernel does not support are dropped when the rule is added to a
 * ruleset, which is the only place that can know the ABI.
 */
class LandlockFilesystemRule {
public:
    LandlockFilesystemRule(std::string path, uint64_t accessMask)
        : _path(std::move(path)), _accessMask(accessMask) {}

    /** Helper factory for granting READ_FILE and READ_DIR rights on `path`. */
    static LandlockFilesystemRule readOnly(std::string path,
                                           boost::optional<uint64_t> extraRights = boost::none);

    /**
     * Helper factory for granting the following rights on `path`:
     * WRITE_FILE, TRUNCATE, REMOVE_DIR, REMOVE_FILE, MAKE_DIR, MAKE_REG, MAKE_SOCK, MAKE_CHAR,
     * MAKE_FIFO, MAKE_BLOCK, MAKE_SYM.
     */
    static LandlockFilesystemRule readWrite(std::string path,
                                            boost::optional<uint64_t> extraRights = boost::none);

    /**
     * Parses one security.landlock.additionalPathRules entry, `<access_grants_csv>:<path>`.
     * <access_grants_csv> is a comma-separated list of access rights, each either a
     * LANDLOCK_ACCESS_FS_* name from the Landlock uAPI without that prefix (READ_FILE,
     * MAKE_DIR, ...), parsed case-insensitively, or one of the aliases:
     *
     *   r  - READ_FILE, READ_DIR
     *   rw - READ_FILE, WRITE_FILE, TRUNCATE, READ_DIR, REMOVE_FILE, REMOVE_DIR, every MAKE_*
     *
     * The aliases name the full set; on a path that is not a directory, addPathRule() keeps
     * only the rights that make sense on a file (r: READ_FILE; rw: READ_FILE, WRITE_FILE,
     * TRUNCATE), so no alias needs to know the path's type here. The grants union.
     *
     * <path> is absolute. Only the first colon separates the two, so a path may contain
     * colons of its own; entries are never split on any other character, so a path may
     * contain commas too.
     *
     * The path is taken exactly as written -- the kernel resolves it when the rule is
     * added, and a path absent on this system grants nothing (see addPathRule). A
     * malformed entry is reported as InvalidOptions naming the entry, rather than
     * skipped, so that a typo cannot quietly narrow the policy an operator asked for.
     * Rights the active policy version does not handle are accepted here and dropped when
     * the rule is added: they are not restricted, so there is nothing to grant.
     */
    static StatusWith<LandlockFilesystemRule> fromConfigString(std::string_view entry);

    /**
     * Returns a copy of this rule that asks for the path it resolves to to be pinned, by holding
     * the descriptor open for the life of the ruleset.
     *
     * A rule binds to the inode its path resolves to, and Landlock keeps that inode alive. What it
     * does not keep is the kernel's cached record of which name maps to that inode (a "dentry").
     * The kernel throws those records away whenever it wants the memory back, which on a durable
     * filesystem is harmless: the mapping is also recorded on disk, so the next lookup of the path
     * finds the same inode again. Under procfs there is no such second copy; that cached dentry
     * is the only thing that ever said which inode this path meant. Once it is gone, the next
     * lookup of the path invents a brand new inode, which carries no rule, and the rule silently
     * stops applying. Holding the descriptor open keeps the record in use, so the kernel will not
     * throw it away.
     *
     * So use this only on a pseudo filesystem, and only where the rule must keep working for the
     * life of the process.
     */
    LandlockFilesystemRule withPinnedPath() const {
        LandlockFilesystemRule pinned = *this;
        pinned._pinPath = true;
        return pinned;
    }

    const std::string& path() const {
        return _path;
    }

    uint64_t accessMask() const {
        return _accessMask;
    }

    /** Whether this rule asked for its path to be pinned. */
    bool pinsPath() const {
        return _pinPath;
    }

private:
    std::string _path;
    uint64_t _accessMask;
    bool _pinPath = false;
};

/**
 * Stands in for the landlock_add_rule() syscall while a ScopedLandlockAddRuleHook is alive. It is
 * given the ruleset descriptor and the access mask the rule would grant. Returns 0 for success,
 * or non-zero (having set errno) for failure, like the syscall.
 *
 * The kernel offers no way to read a ruleset's rules back, so this is the only place a test can
 * observe what addPathRule() decided to grant. The hook replaces the syscall only: the ruleset
 * itself is still created by the kernel, so tests that use it still need Landlock support.
 */
using LandlockAddRuleHook = std::function<long(int rulesetFd, uint64_t allowedAccess)>;

/**
 * Installs `hook` for addPathRule() for the lifetime of this object. Only one may exist at a
 * time, and it is not synchronized: use it from unit tests that add rules on a single thread.
 */
class ScopedLandlockAddRuleHook {
public:
    explicit ScopedLandlockAddRuleHook(LandlockAddRuleHook hook);
    ~ScopedLandlockAddRuleHook();

    ScopedLandlockAddRuleHook(const ScopedLandlockAddRuleHook&) = delete;
    ScopedLandlockAddRuleHook& operator=(const ScopedLandlockAddRuleHook&) = delete;

private:
    LandlockAddRuleHook _hook;
};

/**
 * Owns one Landlock ruleset file descriptor and wraps the three Landlock
 * syscalls: landlock_create_ruleset() (in create()), landlock_add_rule() (in
 * addPathRule()) and landlock_restrict_self() (in restrictSelf()).
 *
 * Filesystem-only for now: the ruleset handles no network or scope
 * restrictions.
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

    ~LandlockRuleset();

    /**
     * Probes the Landlock ABI and creates a ruleset handling the filesystem access rights of
     * `policy`, masked to the probed ABI. Fails with a descriptive Status when the kernel
     * lacks or has disabled Landlock.
     */
    static StatusWith<std::unique_ptr<LandlockRuleset>> create(
        const LandlockPolicy& policy = LandlockPolicy::latest());

    /**
     * Returns the uAPI names of the filesystem rights in `mask` that
     * are directly cross-referenceable against <linux/landlock.h>.
     */
    static std::vector<std::string_view> fsAccessRightNames(uint64_t mask);

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
     * `internal` says the rule is one the server derived for itself, as opposed to one an
     * operator wrote in security.landlock.additionalPathRules. The server's own rules never
     * need certain access rights, so an internal rule that asks for either is a programming
     * error and process-fatal; only an operator's rule can grant them, and then it is theirs
     * to grant.
     *
     * Must not be called once restrictSelf() has been called (process-fatal).
     */
    Status addPathRule(const LandlockFilesystemRule& rule, bool internal = true);

    /**
     * The point of no return: enforces the ruleset on the current process,
     * permanently and inherited across fork()/execve(). Call once, after every
     * rule has been added; from this call on, addPathRule() is forbidden.
     *
     * Must not be called twice (process-fatal): the policy is already permanent.
     */
    Status restrictSelf();

private:
    const int _rulesetFd;
    const int _abi;
    const uint64_t _requestedFsAccess;
    const uint64_t _handledFsAccess;
    bool _restricted = false;
    // O_PATH descriptors held open for the life of this object, one per rule on a pseudo
    // filesystem, so the inode a rule is attached to cannot be reclaimed and regenerated
    // underneath it. See addPathRule. Closed only by the destructor, and the enforcing
    // ruleset is never destroyed, so in practice these last as long as the process.
    std::vector<int> _pinnedPathFds;
};

// Probe the running kernel's Landlock ABI version, via landlock_create_ruleset(2)'s
// LANDLOCK_CREATE_RULESET_VERSION query -- the documented feature-detection
// call. Fails when the kernel lacks Landlock (pre-5.13) or has it disabled in its LSM stack.
StatusWith<long> landlockAbiVersion();

/**
 * Checks the syntax of every security.landlock.additionalPathRules entry, for the
 * option's IDL validator. Runs during option parsing, so a malformed entry is rejected
 * whatever the sandbox mode is and whatever the running kernel supports -- otherwise a
 * typo would sit unnoticed until someone enabled the sandbox on a Landlock kernel.
 *
 * Syntax only: whether each path exists is a question for the moment the rules are
 * applied, long after options are parsed.
 */
Status validateLandlockAdditionalPathRules(const std::vector<std::string>& entries);

}  // namespace mongo

#endif  // defined(__linux__)
