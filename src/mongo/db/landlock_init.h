// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// The staged assembly of the Landlock filesystem policy (Linux only).
//
// The paths a server needs are not all known to one library, so the policy is
// not written in one place: the community policy lives in landlock_init.cpp and
// the enterprise-only paths in the enterprise module's landlock_init.cpp. This
// header is the seam between them. See mongo/util/landlock/landlock.h for what
// the sandbox is and for the ruleset API the rules below are handed to.

#include "mongo/util/landlock/landlock.h"

#include <string>
#include <vector>

#if defined(__linux__)

namespace mongo {

/**
 * The ruleset the server's filesystem policy is being assembled into, or nullptr
 * when this process is not building one: sandboxing is disabled
 * (security.landlock.mode, which it is by default) or the kernel lacks Landlock.
 *
 * The policy is assembled between two startup initializers, so that libraries
 * which know about paths the server needs can contribute rules without knowing
 * about each other:
 *
 *   BeginLandlockSandboxInitialization  creates the ruleset and adds the
 *                                       community policy
 *                                       every other contributor runs in between
 *   EndLandlockSandboxInitialization    enforces the ruleset (the point of no
 *                                       return)
 *
 * A contributor therefore names "BeginLandlockSandboxInitialization" as a
 * prerequisite and "EndLandlockSandboxInitialization" as a dependent, checks
 * this function for a ruleset, and adds its rules through
 * addGlobalLandlockRules(). A contribution arriving after enforcement is caught
 * rather than quietly tolerated: it trips addPathRule()'s invariant. A
 * contributor outside this file must be guarded on __linux__ as everything here
 * is, since naming an initializer that was never registered makes every
 * initializer in the process fail.
 *
 * Startup only, and single-threaded: these are called from initializers, before
 * any thread that could race them exists, and nothing here is synchronized.
 *
 * The returned pointer stays valid for the life of the process, deliberately
 * outliving enforcement so that a late rule trips the invariant above instead of
 * silently never applying.
 */
LandlockRuleset* globalLandlockRuleset();

/**
 * Grants `rules` in the global ruleset, the way every contribution to the policy
 * should: a rule whose path is empty is skipped, since an empty path marks a
 * feature not in use under the current configuration and so grants nothing, and
 * a rule the kernel rejects is fatal -- an operator who asked for a sandbox gets
 * either the whole policy or no server, never a policy quietly missing paths the
 * server needs.
 *
 * Must not be called unless globalLandlockRuleset() returned non-null
 * (process-fatal): there would be nothing to add to, so a caller that skipped
 * that check is either contributing outside the staging window or ignoring that
 * no sandbox is being built.
 */
void addGlobalLandlockRules(const std::vector<LandlockFilesystemRule>& rules);

/**
 * Helpers for turning a configured path (a startup option's value) into the path
 * a rule should bind to. Shared by every policy contributor so they all describe
 * paths identically; see the implementations in landlock_init.cpp for the
 * reasoning behind each. Every one of them maps an unset option (an empty
 * string) to an empty path, which addGlobalLandlockRules() skips.
 */
namespace landlock_policy {

/** A configured path, made absolute against the startup cwd and canonical. */
std::string resolvedConfigPath(const std::string& path);

/**
 * The parent directory of a configured file path, for files the server creates,
 * replaces or re-reads after the sandbox engages (log files, credentials): a rule
 * cannot bind to a file that does not exist yet, and one that binds to the file
 * would go stale on the first rename-and-recreate rotation.
 */
std::string configParentDir(const std::string& path);

/**
 * A configured directory the server creates itself when missing: the directory
 * when it exists, otherwise its parent, so that the creation is permitted too.
 */
std::string configDirCreatedIfMissing(const std::string& path);

/**
 * The value of a string-valued startup option, or empty when unset -- including
 * when the option is one this binary never registered, so a policy can describe
 * another module's paths without linking against it. Naming an option of any
 * other type is a programming error and takes the process down (the option
 * environment throws on the type mismatch, and nothing catches it on the way out
 * of an initializer).
 */
std::string startupOptionValue(const std::string& key);

/**
 * The same, for a path named by a string-valued setParameter given at startup,
 * since some paths (the audit encryption header file, the message filter plugin)
 * are configured that way rather than by a config option.
 */
std::string setParameterStartupValue(const std::string& name);

}  // namespace landlock_policy

}  // namespace mongo

#endif  // defined(__linux__)
