// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// Landlock self-sandboxing for mongod and mongos (Linux only).
//
// Landlock is a Linux LSM that lets an unprivileged process irreversibly drop
// its own ambient filesystem rights. When enabled via --landlock
// (security.landlock.enabled), the server declares the set of path hierarchies it
// legitimately needs at startup (e.g. dbPath read-write, system libraries
// read-only) and the kernel denies every other filesystem access for the
// lifetime of the process and its descendants.
//
// The kernel-facing details (the Landlock uAPI struct layouts, access-right bits
// and syscall numbers, and the mapping from ABI version to supported rights) are
// deliberately confined to landlock.cpp: callers describe what they need with
// the LandlockFilesystemRule factories below and never handle raw rights.

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>

#if defined(__linux__)

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
    static LandlockFilesystemRule readOnly(std::string path);

    /** Full read and mutate rights, for data hierarchies. */
    static LandlockFilesystemRule readWrite(std::string path);

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
     * Probes the Landlock ABI and creates a ruleset handling every filesystem
     * access right this build knows about, masked to the probed ABI. Fails with a
     * descriptive Status when the kernel lacks or has disabled Landlock.
     */
    static StatusWith<std::unique_ptr<LandlockRuleset>> create();

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
     * Must not be called once restrictSelf() has been called (process-fatal).
     */
    Status addPathRule(const LandlockFilesystemRule& rule);

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
};

// Probe the running kernel's Landlock ABI version, via landlock_create_ruleset(2)'s
// LANDLOCK_CREATE_RULESET_VERSION query -- the documented feature-detection
// call. Fails when the kernel lacks Landlock (pre-5.13) or has it disabled in its LSM stack.
StatusWith<long> landlockAbiVersion();

}  // namespace mongo

#endif  // defined(__linux__)
