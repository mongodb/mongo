/**
 * These tests check the version comparison logic in the multiversion test support code.
 *
 * In particular, it tests that the shell version (returned by version()) compares equal to
 * "latest", not equal to "last-stable", and x.y compares equal to x.y.z, but that x.w does
 * not.
 */
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

    // The current version is in the 3.3 series. This has to be changed very time we bump
    // the major version pair, but it provides a useful test of assumptions.
    assertBinVersionsEqual("3.3", version());

    // "latest" is the same version as the shell, "last-stable" is not.
    assertBinVersionsEqual("latest", version());
    assertBinVersionsEqual("", "latest");
    assertBinVersionsEqual("", version());
    assertBinVersionsNotEqual("latest", "last-stable");
    assertBinVersionsNotEqual("last-stable", version());

    // 3.2 means 3.2.z for any value of z. It does not mean 3.0 or 3.0.w.
    assertBinVersionsEqual("3.2", "3.2.4");
    assertBinVersionsEqual("3.2.4", "3.2");
    assertBinVersionsNotEqual("3.2", "3.0");
    assertBinVersionsNotEqual("3.0.9", "3.2.9");

    // Prohibit versions that don't have at least two components (3 is no good, 3.2 is).
    assert.throws(MongoRunner.areBinVersionsTheSame, ["3", "3.2"]);
    assert.throws(MongoRunner.areBinVersionsTheSame, ["3.2", "3"]);
}());
