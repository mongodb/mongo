/**
 * This is an extension of upgrade_enables_extension_foo.js that only runs in auth scenarios. See
 * the header comment in that file for an explanation about expected behavior.
 *
 * TODO SERVER-109450 Delete this test once auth and non-auth have the same behavior.
 *
 * Extensions are only available on Linux machines.
 * @tags: [
 *   requires_auth,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 * ]
 */
import {isLinux} from "jstests/libs/os_helpers.js";
import {
    assertFooStageAccepted,
    assertFooStageRejected,
    fooExtensionNodeOptions,
    setupCollection
} from "jstests/multiVersion/genericBinVersion/extensions_api/upgrade_enables_extension_foo.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {
    testPerformUpgradeSharded
} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

testPerformUpgradeReplSet({
    upgradeNodeOptions: fooExtensionNodeOptions,
    setupFn: setupCollection,
    whenFullyDowngraded: assertFooStageRejected,
    whenSecondariesAreLatestBinary: assertFooStageRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertFooStageAccepted,
    whenFullyUpgraded: assertFooStageAccepted,
});

testPerformUpgradeSharded({
    upgradeNodeOptions: fooExtensionNodeOptions,
    setupFn: setupCollection,
    whenFullyDowngraded: assertFooStageRejected,
    whenOnlyConfigIsLatestBinary: assertFooStageRejected,
    whenSecondariesAndConfigAreLatestBinary: assertFooStageRejected,
    whenMongosBinaryIsLastLTS: assertFooStageRejected,
    whenBinariesAreLatestAndFCVIsLastLTS: assertFooStageAccepted,
    whenFullyUpgraded: assertFooStageAccepted,
});
