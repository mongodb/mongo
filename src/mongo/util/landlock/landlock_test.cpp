// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/landlock/landlock.h"

#include "mongo/base/error_codes.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/wait.h>

namespace mongo {
namespace {

// The Landlock uAPI bits, mirrored here on purpose rather than shared with the
// implementation's own constants: asserting a profile against the same constant that
// builds it would keep passing if a profile were ever widened. These values are stable
// kernel ABI (see <linux/landlock.h>).
constexpr uint64_t kExecute = 1ULL << 0;
constexpr uint64_t kWriteFile = 1ULL << 1;
constexpr uint64_t kReadFile = 1ULL << 2;
constexpr uint64_t kReadDir = 1ULL << 3;
constexpr uint64_t kMakeReg = 1ULL << 8;
constexpr uint64_t kRefer = 1ULL << 13;
constexpr uint64_t kTruncate = 1ULL << 14;
constexpr uint64_t kIoctlDev = 1ULL << 15;

// Landlock is a kernel feature, so anything needing a real ruleset is conditional on
// this host providing one. Rule construction and config parsing need no kernel and run
// everywhere.
bool kernelHasLandlock() {
    return landlockAbiVersion().isOK();
}

LandlockFilesystemRule parseOrFail(std::string_view entry) {
    auto swRule = LandlockFilesystemRule::fromConfigString(entry);
    ASSERT_OK(swRule.getStatus());
    return std::move(swRule.getValue());
}

void assertRejected(std::string_view entry) {
    auto swRule = LandlockFilesystemRule::fromConfigString(entry);
    ASSERT_NOT_OK(swRule.getStatus());
    ASSERT_EQ(ErrorCodes::InvalidOptions, swRule.getStatus().code());
    if (!entry.empty()) {
        // The message must name the offending entry: with a list of them, it is the only
        // thing telling an operator which one to go and fix. Matching the quoted form
        // also catches the quoting being dropped.
        ASSERT_STRING_CONTAINS(swRule.getStatus().reason(), "'" + std::string{entry} + "'");
    }
}

//
// Access profiles.
//

TEST(LandlockFilesystemRule, ReadOnlyGrantsReadsOnly) {
    ASSERT_EQ(kReadFile | kReadDir, LandlockFilesystemRule::readOnly("/x").accessMask());
}

TEST(LandlockFilesystemRule, ReadWriteAddsMutationToReads) {
    const auto readOnly = LandlockFilesystemRule::readOnly("/x").accessMask();
    const auto readWrite = LandlockFilesystemRule::readWrite("/x").accessMask();
    ASSERT_EQ(readOnly, readOnly & readWrite);
    ASSERT_NE(0u, readWrite & kWriteFile);
    ASSERT_NE(0u, readWrite & kTruncate);
    ASSERT_NE(0u, readWrite & kMakeReg);
}

// The policy promises these three stay denied for the life of the process: the ruleset
// handles them, but no rule may grant them back.
TEST(LandlockFilesystemRule, NoProfileGrantsExecuteReferOrIoctl) {
    const uint64_t forbidden = kExecute | kRefer | kIoctlDev;
    ASSERT_EQ(0u, LandlockFilesystemRule::readOnly("/x").accessMask() & forbidden);
    ASSERT_EQ(0u, LandlockFilesystemRule::readWrite("/x").accessMask() & forbidden);
}

TEST(LandlockFilesystemRule, PathIsStoredVerbatim) {
    ASSERT_EQ("/etc/pki", LandlockFilesystemRule::readOnly("/etc/pki").path());
    ASSERT_EQ("/etc/pki/", LandlockFilesystemRule::readWrite("/etc/pki/").path());
}

//
// The additionalPathRules DSL.
//

TEST(LandlockConfigString, ParsesBothAccessSpecifiers) {
    const auto readRule = parseOrFail("r:/etc/pki/ca-trust");
    ASSERT_EQ("/etc/pki/ca-trust", readRule.path());
    ASSERT_EQ(LandlockFilesystemRule::readOnly("/x").accessMask(), readRule.accessMask());

    const auto writeRule = parseOrFail("rw:/srv/site-data");
    ASSERT_EQ("/srv/site-data", writeRule.path());
    ASSERT_EQ(LandlockFilesystemRule::readWrite("/x").accessMask(), writeRule.accessMask());
}

// The kernel resolves the path when the rule is added, so parsing hands it over
// untouched: no normalizing, no trimming, no unquoting.
TEST(LandlockConfigString, PathSurvivesParsingUnchanged) {
    ASSERT_EQ("/etc/pki/ca-trust/", parseOrFail("r:/etc/pki/ca-trust/").path());
    ASSERT_EQ("/", parseOrFail("rw:/").path());
    ASSERT_EQ("/a/./b/../c", parseOrFail("r:/a/./b/../c").path());
    ASSERT_EQ("/spaced out/dir", parseOrFail("r:/spaced out/dir").path());
    ASSERT_EQ("/quoted\"dir", parseOrFail("r:/quoted\"dir").path());
    ASSERT_EQ("/trailing ", parseOrFail("r:/trailing ").path());
}

// Only the first colon separates access from path. Paths legally contain colons, and
// commas -- entries are never split on either, so one entry is exactly one rule and the
// separator is the YAML list element (or a repeat of the command-line option).
TEST(LandlockConfigString, PathMayContainColonsAndCommas) {
    ASSERT_EQ("/var/lib/odd:name", parseOrFail("rw:/var/lib/odd:name").path());
    ASSERT_EQ("/usr/bin/foo,bar", parseOrFail("r:/usr/bin/foo,bar").path());
    ASSERT_EQ("/a,rw:/b", parseOrFail("r:/a,rw:/b").path());
}

TEST(LandlockConfigString, RejectsMissingPrefix) {
    assertRejected("/etc/pki");
    assertRejected("");
    assertRejected("r");
    assertRejected("rw");
}

TEST(LandlockConfigString, RejectsMissingPath) {
    assertRejected("r:");
    assertRejected("rw:");
}

// An empty specifier gets its own diagnostic rather than being reported as an unknown
// one, which would print an unhelpful empty name.
TEST(LandlockConfigString, RejectsMissingAccessSpecifier) {
    assertRejected(":/tmp");
    assertRejected(":");

    const auto status = LandlockFilesystemRule::fromConfigString(":/tmp").getStatus();
    ASSERT_STRING_CONTAINS(status.reason(), "no access specifier");
    ASSERT_STRING_OMITS(status.reason(), "unknown access specifier");
}

TEST(LandlockConfigString, RejectsUnknownAccessSpecifier) {
    assertRejected("w:/tmp");
    assertRejected("rwx:/tmp");
    assertRejected("read:/tmp");
    assertRejected("r :/tmp");
    // Case-sensitive on purpose: accepting "R" would invite "RW" and "Rw" too.
    assertRejected("R:/tmp");
    assertRejected("RW:/tmp");
}

// A relative path resolves against whatever directory the process is in -- "/" once
// --fork has chdir'd -- so it can never mean what the operator wrote.
TEST(LandlockConfigString, RejectsRelativePath) {
    assertRejected("r:etc/pki");
    assertRejected("rw:./data");
    assertRejected("rw:../data");
    assertRejected("r:~/data");
    assertRejected("r: /leading-space");
}

TEST(LandlockConfigString, ValidatorAcceptsEmptyAndValidLists) {
    ASSERT_OK(validateLandlockAdditionalPathRules({}));
    ASSERT_OK(validateLandlockAdditionalPathRules({"r:/etc/pki", "rw:/srv/data", "r:/"}));
}

TEST(LandlockConfigString, ValidatorReportsTheOffendingEntry) {
    const auto status =
        validateLandlockAdditionalPathRules({"r:/etc/pki", "nonsense", "rw:/srv/data"});
    ASSERT_NOT_OK(status);
    ASSERT_EQ(ErrorCodes::InvalidOptions, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'nonsense'");
}

//
// Ruleset construction. These need a kernel that provides Landlock.
//

TEST(LandlockRuleset, CreateReportsAbiAndHandledRights) {
    if (!kernelHasLandlock()) {
        // No ruleset can be built without kernel support, so say so rather than
        // passing this test vacuously.
        ASSERT_NOT_OK(LandlockRuleset::create().getStatus());
        return;
    }
    auto ruleset = unittest::assertGet(LandlockRuleset::create());

    ASSERT_GTE(ruleset->abiVersion(), 1);
    ASSERT_NE(0u, ruleset->handledFsAccess());
    // Whatever this ABI cannot restrict must not also be reported as handled.
    ASSERT_EQ(0u, ruleset->handledFsAccess() & ruleset->degradedFsAccess());
    // EXECUTE is handled -- denied by default -- even though no rule ever grants it.
    ASSERT_NE(0u, ruleset->handledFsAccess() & kExecute);
}

TEST(LandlockRuleset, RightsAreNamedForDiagnostics) {
    ASSERT_EQ(0u, LandlockRuleset::fsAccessRightNames(0).size());
    const auto names = LandlockRuleset::fsAccessRightNames(kReadFile | kWriteFile);
    ASSERT_EQ(2u, names.size());
}

// A path that is not there is not an error: the derived policy lists every path any
// configuration could need, and hierarchies absent on this host simply get no grant.
TEST(LandlockRuleset, AddPathRuleAcceptsAbsentPath) {
    if (!kernelHasLandlock()) {
        return;
    }
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    ASSERT_OK(ruleset->addPathRule(
        LandlockFilesystemRule::readWrite("/definitely/not/a/real/path/for/tests")));
}

// An empty path marks a feature the current configuration does not use. The policy
// filters those out before adding them, but addPathRule tolerates one either way:
// open("") fails with ENOENT, the same skip as any absent path.
TEST(LandlockRuleset, AddPathRuleAcceptsEmptyPath) {
    if (!kernelHasLandlock()) {
        return;
    }
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule::readOnly("")));
    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule::readWrite("")));
}

// Directory-shaped rights are dropped for a non-directory rather than making the kernel
// reject the rule outright (EINVAL).
TEST(LandlockRuleset, AddPathRuleAcceptsFilesAndDirectories) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_paths");
    const std::string filePath = tempDir.path() + "/a_file";
    const int fd = ::open(filePath.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    ASSERT_GTE(fd, 0);
    ::close(fd);

    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule::readWrite(tempDir.path())));
    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule::readWrite(filePath)));
}

//
// Conflicting rules for the same path.
//

// Landlock unions the rights granted to a path, so naming the same hierarchy twice is
// legal however the two rules disagree, and in either order.
TEST(LandlockRuleset, ConflictingRulesForTheSamePathAreAccepted) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_conflict");
    const auto& dir = tempDir.path();

    auto widening = unittest::assertGet(LandlockRuleset::create());
    ASSERT_OK(widening->addPathRule(LandlockFilesystemRule::readOnly(dir)));
    ASSERT_OK(widening->addPathRule(LandlockFilesystemRule::readWrite(dir)));

    auto narrowing = unittest::assertGet(LandlockRuleset::create());
    ASSERT_OK(narrowing->addPathRule(LandlockFilesystemRule::readWrite(dir)));
    ASSERT_OK(narrowing->addPathRule(LandlockFilesystemRule::readOnly(dir)));

    auto repeated = unittest::assertGet(LandlockRuleset::create());
    ASSERT_OK(repeated->addPathRule(LandlockFilesystemRule::readOnly(dir)));
    ASSERT_OK(repeated->addPathRule(LandlockFilesystemRule::readOnly(dir)));
}

// Whether the union actually widens can only be observed by enforcing the policy, which
// is irreversible -- so it happens in a forked child. The child adds `rules`, enforces,
// then tries to create a file under `dir`; its exit code reports whether the write was
// permitted. The parent is untouched: Landlock restricts the calling process and its
// future descendants, never its parent.
enum class WriteOutcome { kPermitted = 0, kDenied = 1, kSetupFailed = 2 };

WriteOutcome writeAllowedUnderRules(const std::string& dir,
                                    const std::vector<LandlockFilesystemRule>& rules) {
    const std::string target = dir + "/probe";
    const pid_t pid = fork();
    invariant(pid >= 0, "failed to fork a Landlock probe child");

    if (pid == 0) {
        // Past restrictSelf() the sandbox is in force for this child, so this stays on
        // raw syscalls and _exit(): no allocation, no logging, no atexit handlers.
        auto swRuleset = LandlockRuleset::create();
        if (!swRuleset.isOK()) {
            _exit(static_cast<int>(WriteOutcome::kSetupFailed));
        }
        auto& ruleset = *swRuleset.getValue();
        for (const auto& rule : rules) {
            if (!ruleset.addPathRule(rule).isOK()) {
                _exit(static_cast<int>(WriteOutcome::kSetupFailed));
            }
        }
        if (!ruleset.restrictSelf().isOK()) {
            _exit(static_cast<int>(WriteOutcome::kSetupFailed));
        }
        const int fd = ::open(target.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        _exit(static_cast<int>(fd >= 0 ? WriteOutcome::kPermitted : WriteOutcome::kDenied));
    }

    int status = 0;
    invariant(::waitpid(pid, &status, 0) == pid);
    invariant(WIFEXITED(status), "Landlock probe child did not exit normally");
    ::unlink(target.c_str());
    return static_cast<WriteOutcome>(WEXITSTATUS(status));
}

TEST(LandlockRuleset, ConflictingRulesUnionTheirRights) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_union");
    const auto& dir = tempDir.path();

    // The control: read-only alone must genuinely deny the write. Without it, every
    // case below would pass just as happily against a sandbox that never engaged.
    ASSERT_EQ(WriteOutcome::kDenied,
              writeAllowedUnderRules(dir, {LandlockFilesystemRule::readOnly(dir)}));

    ASSERT_EQ(WriteOutcome::kPermitted,
              writeAllowedUnderRules(dir, {LandlockFilesystemRule::readWrite(dir)}));

    // Read-write added after read-only widens the grant...
    ASSERT_EQ(
        WriteOutcome::kPermitted,
        writeAllowedUnderRules(
            dir, {LandlockFilesystemRule::readOnly(dir), LandlockFilesystemRule::readWrite(dir)}));

    // ...and the reverse order agrees: a later, narrower rule does not revoke rights
    // already granted for that path.
    ASSERT_EQ(
        WriteOutcome::kPermitted,
        writeAllowedUnderRules(
            dir, {LandlockFilesystemRule::readWrite(dir), LandlockFilesystemRule::readOnly(dir)}));
}

//
// Ordering invariants. Death tests fork, so the sandbox enforced here dies with the
// child rather than restricting the test process.
//

DEATH_TEST(LandlockRulesetDeathTest, AddPathRuleAfterRestrictSelf, "after restrictSelf()") {
    if (!kernelHasLandlock()) {
        // The body must die for the test to pass; say why when there is no kernel.
        invariant(false, "after restrictSelf(): no Landlock on this kernel");
    }
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    uassertStatusOK(ruleset->restrictSelf());
    (void)ruleset->addPathRule(LandlockFilesystemRule::readOnly("/tmp"));
}

DEATH_TEST(LandlockRulesetDeathTest, RestrictSelfTwice, "restrictSelf() twice") {
    if (!kernelHasLandlock()) {
        invariant(false, "restrictSelf() twice: no Landlock on this kernel");
    }
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    uassertStatusOK(ruleset->restrictSelf());
    (void)ruleset->restrictSelf();
}

}  // namespace
}  // namespace mongo
