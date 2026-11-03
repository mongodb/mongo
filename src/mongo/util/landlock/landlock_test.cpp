// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/landlock/landlock.h"

#include "mongo/base/error_codes.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <boost/filesystem/operations.hpp>
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
constexpr uint64_t kRemoveDir = 1ULL << 4;
constexpr uint64_t kRemoveFile = 1ULL << 5;
constexpr uint64_t kMakeReg = 1ULL << 8;
// MAKE_CHAR, MAKE_DIR, MAKE_REG, MAKE_SOCK, MAKE_FIFO, MAKE_BLOCK, MAKE_SYM: bits 6 through 12.
constexpr uint64_t kMakeAll = ((1ULL << 13) - 1) & ~((1ULL << 6) - 1);
constexpr uint64_t kRefer = 1ULL << 13;
constexpr uint64_t kTruncate = 1ULL << 14;
constexpr uint64_t kIoctlDev = 1ULL << 15;

// Every file system access right the uAPI defines as of ABI 5, the newest this binary knows about.
constexpr uint64_t kKnownFsRights = (1ULL << 16) - 1;

// Rights exclusive only to directories
constexpr uint64_t kKnownDirOnlyFsRights = kReadDir | kRemoveDir | kRemoveFile | kMakeAll | kRefer;

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

// Execute and device ioctls stay denied for the life of the process: the ruleset handles
// them, and no default profile grants them back.
TEST(LandlockFilesystemRule, NoProfileGrantsExecuteOrIoctl) {
    const uint64_t forbidden = kExecute | kIoctlDev;
    ASSERT_EQ(0u, LandlockFilesystemRule::readOnly("/x").accessMask() & forbidden);
    ASSERT_EQ(0u, LandlockFilesystemRule::readWrite("/x").accessMask() & forbidden);
}

// Moving a file to a different directory is denied unless a rule asks for it, so that the
// hierarchies where it is allowed are named one by one rather than by default. dbpath asks,
// for file-copy-based initial sync.
TEST(LandlockFilesystemRule, ReferIsGrantedOnlyWhenAskedFor) {
    ASSERT_EQ(0u, LandlockFilesystemRule::readOnly("/x").accessMask() & kRefer);
    ASSERT_EQ(0u, LandlockFilesystemRule::readWrite("/x").accessMask() & kRefer);
    ASSERT_NE(0u, LandlockFilesystemRule::readWrite("/x", kRefer).accessMask() & kRefer);
}

// An extra right is added to the profile, not substituted for it: asking for one must not
// quietly drop the rest.
TEST(LandlockFilesystemRule, ExtraRightsAreAddedToTheProfile) {
    ASSERT_EQ(LandlockFilesystemRule::readWrite("/x").accessMask() | kRefer,
              LandlockFilesystemRule::readWrite("/x", kRefer).accessMask());
    ASSERT_EQ(LandlockFilesystemRule::readOnly("/x").accessMask() | kRefer,
              LandlockFilesystemRule::readOnly("/x", kRefer).accessMask());
}

TEST(LandlockFilesystemRule, PathIsStoredVerbatim) {
    ASSERT_EQ("/etc/pki", LandlockFilesystemRule::readOnly("/etc/pki").path());
    ASSERT_EQ("/etc/pki/", LandlockFilesystemRule::readWrite("/etc/pki/").path());
}

//
// The additionalPathRules DSL.
//

TEST(LandlockConfigString, ParsesBothAliases) {
    const auto readRule = parseOrFail("r:/etc/pki/ca-trust");
    ASSERT_EQ("/etc/pki/ca-trust", readRule.path());
    ASSERT_EQ(kReadFile | kReadDir, readRule.accessMask());

    // rw is READ_FILE, WRITE_FILE, TRUNCATE, READ_DIR, REMOVE_*, MAKE_*
    const auto writeRule = parseOrFail("rw:/srv/site-data");
    ASSERT_EQ("/srv/site-data", writeRule.path());
    ASSERT_EQ(kReadFile | kWriteFile | kTruncate | kReadDir | kRemoveFile | kRemoveDir | kMakeAll,
              writeRule.accessMask());
}

TEST(LandlockConfigString, ParsesIndividualRightsCaseInsensitively) {
    ASSERT_EQ(kReadFile | kReadDir, parseOrFail("read_file,read_dir:/x").accessMask());
    ASSERT_EQ(kReadFile | kReadDir, parseOrFail("READ_FILE,Read_Dir:/x").accessMask());
    ASSERT_EQ(kExecute, parseOrFail("execute:/x").accessMask());
    ASSERT_EQ(kIoctlDev, parseOrFail("ioctl_dev:/x").accessMask());
    ASSERT_EQ(kRefer, parseOrFail("refer:/x").accessMask());
}

TEST(LandlockConfigString, EveryRightNamePrintedForDiagnosticsParses) {
    // The names a diagnostic prints are the uAPI names; with the prefix stripped, each must be
    // accepted back, so an operator can copy a right straight from a log line.
    constexpr std::string_view kPrefix = "LANDLOCK_ACCESS_FS_";
    uint64_t seen = 0;
    for (const auto name : LandlockRuleset::fsAccessRightNames(kKnownFsRights)) {
        ASSERT_TRUE(name.starts_with(kPrefix)) << name;
        const auto rule = parseOrFail(std::string{name.substr(kPrefix.size())} + ":/x");
        ASSERT_EQ(1, std::popcount(rule.accessMask())) << name;
        seen |= rule.accessMask();
    }
    ASSERT_EQ(kKnownFsRights, seen);
}

TEST(LandlockConfigString, GrantsUnion) {
    ASSERT_EQ(kReadFile | kReadDir | kWriteFile | kTruncate,
              parseOrFail("r,write_file,truncate:/x").accessMask());
    ASSERT_EQ(parseOrFail("rw:/x").accessMask(), parseOrFail("r,rw:/x").accessMask());
    ASSERT_EQ(kReadFile, parseOrFail("read_file,read_file:/x").accessMask());
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

TEST(LandlockConfigString, RejectsEmptyListElements) {
    assertRejected("r,:/tmp");
    assertRejected(",r:/tmp");
    assertRejected("r,,rw:/tmp");
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
    assertRejected("r, rw:/tmp");
    assertRejected("landlock_access_fs_read_file:/tmp");
    assertRejected("read_file,bogus:/tmp");
    // "MAKE_*" in the documentation is shorthand for the individual MAKE_ rights.
    assertRejected("make_*:/tmp");
    assertRejected("make:/tmp");

    const auto status =
        LandlockFilesystemRule::fromConfigString("read_file,bogus:/tmp").getStatus();
    ASSERT_STRING_CONTAINS(status.reason(), "unknown access specifier 'bogus'");
}

TEST(LandlockConfigString, AliasesAreCaseInsensitive) {
    ASSERT_EQ(parseOrFail("r:/x").accessMask(), parseOrFail("R:/x").accessMask());
    ASSERT_EQ(parseOrFail("rw:/x").accessMask(), parseOrFail("RW:/x").accessMask());
    ASSERT_EQ(parseOrFail("rw:/x").accessMask(), parseOrFail("Rw:/x").accessMask());
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

// Path resolution tests. What a configured path turns into is what a rule ends up bound to, so
// these cover the shapes an operator may write.

TEST(LandlockPath, EmptyStaysEmpty) {
    auto resolved = canonicalizePathForLandlock("", "/base");
    ASSERT_EQ("", resolved.string());
    ASSERT_EQ("", resolved.parent_path().string());
}

TEST(LandlockPath, RelativePathResolvesAgainstTheBase) {
    ASSERT_EQ("/base/data/log", canonicalizePathForLandlock("data/log", "/base").string());
}

TEST(LandlockPath, AbsolutePathIgnoresTheBase) {
    auto resolved = canonicalizePathForLandlock("/var/log/mongodb", "/base");
    ASSERT_EQ("/var/log/mongodb", resolved.string());
    ASSERT_EQ("/var/log", resolved.parent_path().string());
}

TEST(LandlockPath, RootHasNoFilenameToStrip) {
    auto resolved = canonicalizePathForLandlock("/", "/base");
    ASSERT_EQ("/", resolved.string());
    ASSERT_EQ("", resolved.parent_path().string());
}

// Only "." is dropped. ".." is a real component and must resolve, not be mistaken for a
// trailing separator.
TEST(LandlockPath, DotDotResolvesRatherThanBeingStripped) {
    unittest::TempDir tempDir("landlock_path_test");
    const std::string viaParent = tempDir.path() + "/ftdc/..";
    ASSERT_EQ(tempDir.path(), canonicalizePathForLandlock(viaParent, "/base").string());
}

// Test symlinks are resolved to the right path
TEST(LandlockPath, SymbolicLinksResolveCorrectly) {
    namespace fs = boost::filesystem;
    boost::system::error_code ec;

    // create .../landlock_path_test
    unittest::TempDir tempDir("landlock_path_test");
    const auto absTempPath = fs::absolute(tempDir.path(), ec);
    ASSERT(!ec);

    const auto target = absTempPath / "symlink_target";
    const auto symlinkParent = absTempPath / "foo" / "bar";
    const auto symlink = symlinkParent / "symlink";

    // .../landlock_path_test/symlink_target/
    // .../landlock_path_test/foo/bar/symlink -> .../landlock_path_test/symlink_target/
    ASSERT_DOES_NOT_THROW(fs::create_directory(target));
    ASSERT_DOES_NOT_THROW(fs::create_directories(symlinkParent));
    ASSERT_DOES_NOT_THROW(fs::create_directory_symlink(target, symlink));

    // .../landlock_path_test/symlink_target/ resolves to itself
    ASSERT_EQ(target.string(), canonicalizePathForLandlock(target.string(), "/").string());

    // .../landlock_path_test/foo/bar/symlink resolves to .../landlock_path_test/symlink_target/
    ASSERT_EQ(target.string(), canonicalizePathForLandlock(symlink.string(), "/").string());

    // .../landlock_path_test/foo/bar/symlink/.. resolves to .../landlock_path_test/
    ASSERT_EQ(absTempPath.string(),
              canonicalizePathForLandlock((symlink / "..").string(), "/").string());

    // .../landlock_path_test/foo/bar/symlink/../foo/bar resolves to .../landlock_path_test/foo/bar
    ASSERT_EQ(symlinkParent.string(),
              canonicalizePathForLandlock((symlink / ".." / "foo" / "bar").string(), "/").string());
}

// A path that cannot be resolved is a path the policy cannot describe, so it fails rather than
// falling back on a lexical normalization -- which, wherever a symlink is involved, would name a
// different hierarchy than the one the server will actually open.
//
// Provoked with a symlink loop rather than an unsearchable directory: ELOOP is refused for root
// too, so this does not quietly skip itself wherever the tests run privileged.
TEST(LandlockPath, UnresolvablePathThrows) {
    namespace fs = boost::filesystem;

    unittest::TempDir tempDir("landlock_path_test");
    const auto loop = fs::path(tempDir.path()) / "loop";
    ASSERT_DOES_NOT_THROW(fs::create_symlink("loop", loop));

    // The loop is an interior component, which is the shape an operator would actually hit: a
    // configured file whose parent directory is a broken symlink.
    const auto beneath = (loop / "data").string();

    ASSERT_THROWS_CODE(
        canonicalizePathForLandlock(beneath, "/"), DBException, ErrorCodes::OperationFailed);

    // The message has to name the configured path: this runs for every path in the policy, so it
    // is the only thing telling an operator which option to go and fix.
    try {
        canonicalizePathForLandlock(beneath, "/");
        FAIL("expected canonicalizePathForLandlock to throw");
    } catch (const DBException& ex) {
        ASSERT_STRING_CONTAINS(ex.reason(), beneath);
    }
}

// A path whose components do not exist is not a failure: the policy names files the server has
// yet to create, and only the existing prefix can be canonicalized.
TEST(LandlockPath, MissingPathResolvesRatherThanThrowing) {
    unittest::TempDir tempDir("landlock_path_test");
    const auto missing = tempDir.path() + "/not/created/yet";
    ASSERT_EQ(missing, canonicalizePathForLandlock(missing, "/base").string());
}

TEST(LandlockPath, TrailingElementsAreStripped) {
    namespace fs = boost::filesystem;
    boost::system::error_code ec;

    unittest::TempDir tempDir("landlock_path_test");
    auto absTempPath = fs::absolute(tempDir.path(), ec);
    const auto existingDir = absTempPath.string();
    const auto missingDir = (absTempPath / "ftdc").string();

    // test parent_path behavior on dir paths that end with a "/"
    ASSERT_EQ("/data/foo", fs::path("/data/foo/").parent_path().string());

    // test weakly_canonical behavior on dir paths that end with a "/"
    // the "/" is stripped only if the directory exists
    auto resolved = fs::weakly_canonical(fs::path(existingDir + "/"), ec);
    ASSERT(!ec);
    ASSERT_EQ(existingDir, resolved.string());
    resolved = fs::weakly_canonical(fs::path(missingDir + "/"), ec);
    ASSERT(!ec);
    ASSERT_EQ(missingDir + "/", resolved.string());

    // test that canonicalizePathForLandlock can handle trailing separators &
    // redundant path elements on missing paths where weakly_canonicalize cannot.
    const auto suffixes = {"/", "/.", "/./", "/.//", "/foo/..", "/foo//bar/.././../"};
    for (const auto& sfx : suffixes) {
        const auto path = missingDir + sfx;
        ASSERT_EQ(missingDir, canonicalizePathForLandlock(path, "/base").string())
            << "for '" << path << "'";
    }
    // test that canonicalizePathForLandlock can handle redundant path elements for existing paths.
    for (const auto& sfx : suffixes) {
        const auto path = existingDir + sfx;
        ASSERT_EQ(existingDir, canonicalizePathForLandlock(path, "/base").string())
            << "for '" << path << "'";
    }
}

//
// Policy versions. These need no kernel.
//

TEST(LandlockPolicy, VersionsAreContiguousFromOne) {
    const auto policies = LandlockPolicy::all();
    ASSERT_FALSE(policies.empty());
    for (size_t i = 0; i < policies.size(); ++i) {
        ASSERT_EQ(static_cast<int>(i) + 1, policies[i].version());
    }
    ASSERT_EQ(policies.back().version(), LandlockPolicy::latest().version());
}

TEST(LandlockPolicy, RightsNeverShrink) {
    const auto policies = LandlockPolicy::all();
    for (size_t i = 1; i < policies.size(); ++i) {
        const auto& prev = policies[i - 1];
        const auto& next = policies[i];
        // A newer version may only add rights: the previous set must be a subset.
        ASSERT_EQ(prev.handledFsAccess(), prev.handledFsAccess() & next.handledFsAccess())
            << "version " << next.version() << " removes rights handled by " << prev.version();
    }
}

TEST(LandlockPolicy, HandlesOnlyKnownRights) {
    for (const auto& policy : LandlockPolicy::all()) {
        ASSERT_NE(0u, policy.handledFsAccess()) << "version " << policy.version();
        ASSERT_EQ(0u, policy.handledFsAccess() & ~kKnownFsRights) << "version " << policy.version();
    }
}

// Released policies are immutable. If this fails because an entry was edited, add a new
// version instead; only append a row here when a new version is added.
TEST(LandlockPolicy, ReleasedVersionsAreImmutable) {
    struct Released {
        int version;
        uint64_t handledFsAccess;
    };
    // v1: EXECUTE | WRITE_FILE | READ_FILE | READ_DIR | REMOVE_* | MAKE_* | REFER | TRUNCATE |
    // IOCTL_DEV
    constexpr Released kReleased[] = {
        {1, 0xFFFF},
    };
    const auto policies = LandlockPolicy::all();
    ASSERT_GTE(policies.size(), std::size(kReleased));
    for (size_t i = 0; i < std::size(kReleased); ++i) {
        ASSERT_EQ(kReleased[i].version, policies[i].version());
        ASSERT_EQ(kReleased[i].handledFsAccess, policies[i].handledFsAccess())
            << "version " << kReleased[i].version;
    }
}

TEST(LandlockPolicy, ForVersionFindsEachImplementedVersion) {
    for (const auto& policy : LandlockPolicy::all()) {
        const auto* found = unittest::assertGet(LandlockPolicy::forVersion(policy.version()));
        ASSERT_EQ(&policy, found);
    }
}

TEST(LandlockPolicy, ForVersionRejectsUnimplementedVersions) {
    for (long long version : {-1LL, 0LL, LandlockPolicy::latest().version() + 1LL, 1LL << 40}) {
        const auto status = LandlockPolicy::forVersion(version).getStatus();
        ASSERT_EQ(ErrorCodes::InvalidOptions, status.code()) << "version " << version;
    }
    ASSERT_NOT_OK(validateLandlockPolicyVersion(LandlockPolicy::latest().version() + 1));
    ASSERT_OK(validateLandlockPolicyVersion(1));
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

TEST(LandlockRuleset, CreateWithEmptyPolicyFails) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_grants");
    LandlockPolicy emptyPolicy{1, 0};
    ASSERT_THROWS_CODE(LandlockRuleset::create(emptyPolicy), DBException, 13118813);
}

TEST(LandlockRuleset, CreateHandlesNoMoreThanThePolicy) {
    if (!kernelHasLandlock()) {
        return;
    }
    LandlockPolicy smallSet{1,
                            LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                                LANDLOCK_ACCESS_FS_EXECUTE};

    for (const auto& policy : {smallSet, LandlockPolicy::latest()}) {
        auto ruleset = unittest::assertGet(LandlockRuleset::create(policy));
        ASSERT_EQ(0u, ruleset->handledFsAccess() & ~policy.handledFsAccess());
        ASSERT_EQ(policy.handledFsAccess(),
                  ruleset->handledFsAccess() | ruleset->degradedFsAccess());
    }
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

//
// Hook that observes what addPathRule hands the kernel (because the kernel cannot
// report a ruleset's rules back). Rights are compared after masking to what the running
// kernel's ABI handles, so the expectations hold on any ABI that has Landlock.
//
class AddRuleRecorder {
public:
    AddRuleRecorder()
        : _hook([this](int, uint64_t allowedAccess) {
              grants.push_back(allowedAccess);
              return 0L;
          }) {}

    std::vector<uint64_t> grants;

private:
    ScopedLandlockAddRuleHook _hook;
};

TEST(LandlockRuleset, AddPathRuleGrantsTheRulesRightsToADirectory) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_grants");
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    AddRuleRecorder recorder;

    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule::readOnly(tempDir.path())));
    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule::readWrite(tempDir.path())));

    ASSERT_EQ(2u, recorder.grants.size());
    ASSERT_EQ(kReadFile | kReadDir, recorder.grants[0]);
    ASSERT_EQ(LandlockFilesystemRule::readWrite("").accessMask() & ruleset->handledFsAccess(),
              recorder.grants[1]);
}

TEST(LandlockRuleset, AddPathRuleNarrowsAFileGrantToFileRights) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_grants");
    const std::string filePath = tempDir.path() + "/a_file";
    const int fd = ::open(filePath.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    ASSERT_GTE(fd, 0);
    ::close(fd);
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    AddRuleRecorder recorder;

    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule{filePath, kKnownFsRights}, false));

    auto rightsHandled = kKnownFsRights & ruleset->handledFsAccess();
    auto rightsFiltered = rightsHandled & ~kKnownDirOnlyFsRights;

    ASSERT_EQ(1u, recorder.grants.size());
    ASSERT_EQ(recorder.grants[0], rightsFiltered);
}

TEST(LandlockRuleset, AddPathRuleGrantsNothingBeyondTheRulesetsHandledRights) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_grants");
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    AddRuleRecorder recorder;

    // Everything, including rights this ABI may not handle: the kernel would answer EINVAL.
    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule(tempDir.path(), ~uint64_t{0}), false));

    ASSERT_EQ(1u, recorder.grants.size());
    ASSERT_EQ(0u, recorder.grants[0] & ~ruleset->handledFsAccess());
}

TEST(LandlockRuleset, AddPathRuleReportsAFailedAddRule) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_grants");
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    ScopedLandlockAddRuleHook failing([](int, uint64_t) {
        errno = EINVAL;
        return -1L;
    });

    ASSERT_EQ(ErrorCodes::OperationFailed,
              ruleset->addPathRule(LandlockFilesystemRule::readOnly(tempDir.path())).code());
}

//
// Pinning the inode a rule resolved to.
//

// The number of descriptors this process holds open.
size_t openDescriptorCount() {
    size_t count = 0;
    // Counted through /proc rather than by scanning fd numbers, so an unrelated descriptor
    // opened elsewhere in the test binary cannot be mistaken for one of ours.
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        while (const dirent* entry = ::readdir(dir)) {
            if (entry->d_name[0] != '.') {
                ++count;
            }
        }
        ::closedir(dir);
    }
    return count;
}

// withPinnedPath() is what asks for the pin, and it is the only thing that does. A rule binds to
// the inode its path resolved to, and on procfs that path can come to resolve to a different
// inode, silently leaving the rule behind; see addPathRule for the failure this prevents.
TEST(LandlockFilesystemRule, PinningIsOffUntilAskedFor) {
    ASSERT_FALSE(LandlockFilesystemRule::readWrite("/proc/self").pinsPath());
    ASSERT_TRUE(LandlockFilesystemRule::readWrite("/proc/self").withPinnedPath().pinsPath());
}

// Asking for the pin changes nothing else about the rule.
TEST(LandlockFilesystemRule, PinningLeavesThePathAndRightsAlone) {
    const auto plain = LandlockFilesystemRule::readWrite("/proc/self", kRefer);
    const auto pinned = plain.withPinnedPath();
    ASSERT_EQ(plain.path(), pinned.path());
    ASSERT_EQ(plain.accessMask(), pinned.accessMask());
}

// A rule that asked for the pin keeps its O_PATH descriptor open.
TEST(LandlockRuleset, PinnedRuleHoldsItsDescriptorOpen) {
    if (!kernelHasLandlock()) {
        return;
    }
    auto ruleset = unittest::assertGet(LandlockRuleset::create());

    const auto before = openDescriptorCount();
    ASSERT_OK(
        ruleset->addPathRule(LandlockFilesystemRule::readWrite("/proc/self").withPinnedPath()));
    ASSERT_EQ(before + 1, openDescriptorCount());
}

// A rule that did not ask keeps none, which is the default for every rule the policy has apart
// from the one on /proc/<pid>: an open descriptor pins the mount as well as the inode, so
// pinning dbpath would keep its filesystem busy for the life of the server, guarding against a
// reclaim that cannot happen there.
TEST(LandlockRuleset, UnpinnedRuleHoldsNoDescriptor) {
    if (!kernelHasLandlock()) {
        return;
    }
    unittest::TempDir tempDir("landlock_test_pinning");
    auto ruleset = unittest::assertGet(LandlockRuleset::create());

    const auto before = openDescriptorCount();
    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule::readWrite(tempDir.path())));
    ASSERT_EQ(before, openDescriptorCount());
    // Nothing about procfs pins on its own; only the request does.
    ASSERT_OK(ruleset->addPathRule(LandlockFilesystemRule::readWrite("/proc/self")));
    ASSERT_EQ(before, openDescriptorCount());
}

// Destroying an unenforced ruleset releases what it pinned. The enforcing one is never
// destroyed, so this only matters for a startup failure or a test -- but a leak here would
// accumulate a descriptor per rule per ruleset built.
TEST(LandlockRuleset, DestroyingARulesetReleasesPinnedDescriptors) {
    if (!kernelHasLandlock()) {
        return;
    }
    const auto before = openDescriptorCount();
    {
        auto ruleset = unittest::assertGet(LandlockRuleset::create());
        ASSERT_OK(
            ruleset->addPathRule(LandlockFilesystemRule::readWrite("/proc/self").withPinnedPath()));
        ASSERT_OK(ruleset->addPathRule(
            LandlockFilesystemRule::readOnly("/proc/self/task").withPinnedPath()));
        ASSERT_GT(openDescriptorCount(), before);
    }
    ASSERT_EQ(before, openDescriptorCount());
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

DEATH_TEST(LandlockRulesetDeathTest,
           InternalRuleMayNotGrantExecute,
           "requests EXECUTE or IOCTL_DEV") {
    if (!kernelHasLandlock()) {
        invariant(false, "requests EXECUTE or IOCTL_DEV: no Landlock on this kernel");
    }
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    (void)ruleset->addPathRule(LandlockFilesystemRule("/tmp", kReadFile | kExecute));
}

DEATH_TEST(LandlockRulesetDeathTest,
           InternalRuleMayNotGrantIoctlDev,
           "requests EXECUTE or IOCTL_DEV") {
    if (!kernelHasLandlock()) {
        invariant(false, "requests EXECUTE or IOCTL_DEV: no Landlock on this kernel");
    }
    auto ruleset = unittest::assertGet(LandlockRuleset::create());
    (void)ruleset->addPathRule(LandlockFilesystemRule("/tmp", kReadFile | kIoctlDev));
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
