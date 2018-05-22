/**
 * These tests check the version comparison logic in the multiversion test support code.
 *
 * In particular, it tests that the shell version (returned by version()) compares equal to
 * "latest", not equal to "last-stable", and x.y compares equal to x.y.z, but that x.w does
 * not.
 */

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    "use strict";

    function assertBinVersionsEqual(v1, v2) {
        assert(MongoRunner.areBinVersionsTheSame(v1, v2),
               "Expected \"" + v1 + "\" to equal \"" + v2 + "\"");
    }

    function assertBinVersionsNotEqual(v1, v2) {
        assert(!MongoRunner.areBinVersionsTheSame(v1, v2),
               "Expected \"" + v1 + "\" not to equal \"" + v2 + "\"");
    }

    function assertBinVersionComparesHigher(v1, v2) {
        assert.eq(1,
                  MongoRunner.compareBinVersions(v1, v2),
                  "Expected \"" + v1 + "\" to compare higher than \"" + v2 + "\"");
    }

    function assertBinVersionComparesLower(v1, v2) {
        assert.eq(-1,
                  MongoRunner.compareBinVersions(v1, v2),
                  "Expected \"" + v1 + "\" to compare lower than \"" + v2 + "\"");
    }

    function assertBinVersionComparesEqual(v1, v2) {
        assert.eq(0,
                  MongoRunner.compareBinVersions(v1, v2),
                  "Expected \"" + v1 + "\" to compare equal to \"" + v2 + "\"");
    }

    // The current version is in the 3.7 series. This has to be changed very time we bump
    // the major version pair, but it provides a useful test of assumptions.
    assertBinVersionsEqual("4.1", version());
    assertBinVersionComparesEqual("4.1", version());

    // "latest" is the same version as the shell, "last-stable" is not.
    assertBinVersionsEqual("latest", version());
    assertBinVersionsEqual("", "latest");
    assertBinVersionsEqual("", version());

    assertBinVersionComparesEqual("latest", version());
    assertBinVersionComparesEqual("", "latest");
    assertBinVersionComparesEqual("", version());

    assertBinVersionsNotEqual("latest", "last-stable");
    assertBinVersionsNotEqual("last-stable", version());

    assertBinVersionComparesHigher("latest", "last-stable");
    assertBinVersionComparesLower("last-stable", version());

    // 3.2 means 3.2.z for any value of z. It does not mean 3.0 or 3.0.w.
    assertBinVersionsEqual("3.2", "3.2.4");
    assertBinVersionsEqual("3.2.4", "3.2");
    assertBinVersionsNotEqual("3.2", "3.0");
    assertBinVersionsNotEqual("3.0.9", "3.2.9");

    assertBinVersionComparesEqual("3.2", "3.2.4");
    assertBinVersionComparesEqual("3.2.4", "3.2");
    assertBinVersionComparesHigher("3.2", "3.0");
    assertBinVersionComparesLower("3.0.9", "3.2.9");

    assertBinVersionsEqual("3.4", "3.4.0-abcd");
    assertBinVersionsEqual("3.4.0", "3.4.0-abcd");

    assertBinVersionComparesEqual("3.4", "3.4.0-abcd");
    assertBinVersionComparesEqual("3.4.0", "3.4.0-abcd");
    assertBinVersionComparesHigher("3.6.0", "3.4.0-abcd");
    assertBinVersionComparesHigher("4.0.0", "3.6.99-abcd");
    assertBinVersionComparesHigher("3.4.1", "3.4.0-abcd");
    assertBinVersionComparesLower("3.4.0-abc", "3.4.1-xyz");

    // Prohibit versions that don't have at least two components (3 is no good, 3.2 is).
    assert.throws(MongoRunner.areBinVersionsTheSame, ["3", "3.2"]);
    assert.throws(MongoRunner.areBinVersionsTheSame, ["3.2", "3"]);

    // Throw an error when versions differ only by githash.
    assert.throws(MongoRunner.compareBinVersions, ["3.4.1-abc", "3.4.1-xyz"]);
}());
