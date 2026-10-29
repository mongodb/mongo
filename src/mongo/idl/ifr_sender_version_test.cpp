// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/ifr_sender_version.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <vector>

namespace mongo {
namespace {

IFRSenderVersion makeVersion(int major, int minor, int patch, int extra) {
    IFRSenderVersion v;
    v.setMajor(major);
    v.setMinor(minor);
    v.setPatch(patch);
    v.setExtra(extra);
    return v;
}

// A VersionInfoInterface whose numeric components are configurable. Mirrors the encoding produced
// by version_constants_gen.py, where a release-candidate build has a negative 'extra' and a final
// release has 'extra' == 0.
class ConfigurableVersionInfo : public VersionInfoInterface {
public:
    ConfigurableVersionInfo(int major, int minor, int patch, int extra)
        : _major(major), _minor(minor), _patch(patch), _extra(extra) {}

    int majorVersion() const final {
        return _major;
    }
    int minorVersion() const final {
        return _minor;
    }
    int patchVersion() const final {
        return _patch;
    }
    int extraVersion() const final {
        return _extra;
    }
    std::string_view version() const final {
        return "test";
    }
    std::string_view gitVersion() const final {
        return "test";
    }
    std::vector<std::string_view> modules() const final {
        return {};
    }
    std::string_view allocator() const final {
        return "test";
    }
    std::string_view jsEngine() const final {
        return "test";
    }
    std::string_view targetMinOS() const final {
        return "test";
    }
    std::vector<BuildInfoField> buildInfo() const final {
        return {};
    }

private:
    int _major;
    int _minor;
    int _patch;
    int _extra;
};

TEST(IFRSenderVersionTest, ComparisonIsLexicographicOnComponents) {
    // Major dominates.
    ASSERT_LT(makeVersion(8, 9, 9, 0), makeVersion(9, 0, 0, 0));
    // Then minor.
    ASSERT_LT(makeVersion(9, 0, 9, 0), makeVersion(9, 1, 0, 0));
    // Then patch: this is the case that FCV-granularity ("9.0") could not distinguish.
    ASSERT_LT(makeVersion(9, 0, 1, 0), makeVersion(9, 0, 5, 0));

    ASSERT_EQ(makeVersion(9, 0, 5, 0), makeVersion(9, 0, 5, 0));
    ASSERT_NE(makeVersion(9, 0, 1, 0), makeVersion(9, 0, 5, 0));
}

TEST(IFRSenderVersionTest, PreReleaseSortsBeforeFinalRelease) {
    // version_constants_gen.py encodes "9.0.0-rc2" as extra == -23 and the final "9.0.0" as
    // extra == 0, so a release candidate must sort strictly before its final release.
    const auto rc2 = makeVersion(9, 0, 0, -23);
    const auto finalRelease = makeVersion(9, 0, 0, 0);
    ASSERT_LT(rc2, finalRelease);

    // An earlier rc sorts before a later rc (-25 == rc0, -23 == rc2).
    ASSERT_LT(makeVersion(9, 0, 0, -25), makeVersion(9, 0, 0, -23));
}

TEST(IFRSenderVersionTest, OrderingChainAcrossReleasesAndReleaseCandidates) {
    // Encodes an "-rcN" pre-release the same way version_constants_gen.py does: extra == N - 25.
    // A final release has extra == 0.
    auto rc = [](int major, int minor, int patch, int rcNum) {
        return makeVersion(major, minor, patch, rcNum - 25);
    };

    // The full ordering we require. Note that rc2 must sort before rc12 (numeric comparison of the
    // rc number, not a lexicographic string comparison that would place "rc12" before "rc2").
    const std::vector<IFRSenderVersion> ascending{
        makeVersion(8, 3, 4, 0),  // 8.3.4
        makeVersion(8, 3, 5, 0),  // 8.3.5
        rc(9, 0, 0, 0),           // 9.0.0-rc0
        rc(9, 0, 0, 1),           // 9.0.0-rc1
        rc(9, 0, 0, 2),           // 9.0.0-rc2
        rc(9, 0, 0, 12),          // 9.0.0-rc12
        makeVersion(9, 0, 0, 0),  // 9.0.0
        makeVersion(9, 0, 1, 0),  // 9.0.1
        rc(9, 1, 0, 0),           // 9.1.0-rc0
    };

    for (size_t i = 1; i < ascending.size(); ++i) {
        ASSERT_LT(ascending[i - 1], ascending[i])
            << "expected element " << (i - 1) << " to sort before element " << i;
    }
}

TEST(IFRSenderVersionTest, MakeIFRSenderVersionReflectsProvider) {
    ConfigurableVersionInfo provider(9, 0, 5, 0);
    const auto version = makeIFRSenderVersion(provider);
    ASSERT_EQ(version.getMajor(), 9);
    ASSERT_EQ(version.getMinor(), 0);
    ASSERT_EQ(version.getPatch(), 5);
    ASSERT_EQ(version.getExtra(), 0);
    ASSERT_EQ(version, makeVersion(9, 0, 5, 0));
}

TEST(IFRSenderVersionTest, RoundTripsThroughBSON) {
    const auto original = makeVersion(9, 0, 5, -23);
    BSONObjBuilder bob;
    original.serialize(&bob);
    const auto parsed =
        IFRSenderVersion::parse(bob.obj(), IDLParserContext("IFRSenderVersionTest"));
    ASSERT_EQ(parsed, original);
}

}  // namespace
}  // namespace mongo
