/**
 * Tests initializing a mixed version replica set through resmoke.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: replSetGetStatus.
 *   not_allowed_with_signed_security_token,
 *   multiversion_sanity_check,
 *   future_git_tag_incompatible
 * ]
 */

const latestBinVersion = MongoRunner.getBinVersionFor("latest");

if (testingReplication && TestData && TestData.mixedBinVersions) {
    const replSetStatus = db.adminCommand({"replSetGetStatus": 1});
    const members = replSetStatus["members"];
    assert.eq(TestData.mixedBinVersions.length, replSetStatus["members"].length);
    for (let i = 0; i < TestData.mixedBinVersions.length; i++) {
        const conn = new Mongo(members[i]["name"]);
        const admin = conn.getDB("admin");
        const serverStatus = admin.serverStatus();
        const actualVersion = serverStatus["version"];

        const nodeAtLatestVersion = TestData.mixedBinVersions[i] === "new";

        jsTest.log.info("Checking node binary version", {
            actualVersion,
            multiversionBinVersion: TestData.multiversionBinVersion,
            mixedBinVersion: TestData.mixedBinVersions[i],
        });

        if (!nodeAtLatestVersion && TestData.multiversionBinVersion === "last-patch") {
            // For last-patch we can't compute the exact version: the master branch isn't updated
            // when a patch release is cut. So we only assert it differs from latest.
            assert(
                !MongoRunner.areBinVersionsTheSame(actualVersion, latestBinVersion),
                "last-patch node unexpectedly at latest version",
                {actualVersion, latestBinVersion},
            );
        } else {
            const expectedVersion = nodeAtLatestVersion
                ? latestBinVersion
                : MongoRunner.getBinVersionFor(TestData.multiversionBinVersion);
            assert(
                MongoRunner.areBinVersionsTheSame(actualVersion, expectedVersion),
                "node binary version does not match expected version",
                {actualVersion, expectedVersion},
            );
        }
    }
} else {
    jsTestLog(
        "This tests initializing a mixed version replica set through resmoke. Skipping test run" +
            " because testingReplication and TestData.mixedBinVersion are not set.",
    );
}
