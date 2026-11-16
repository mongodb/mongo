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
            // when a patch release is cut. So we only assert the node is not running the latest
            // *build*. A DSC release and mainline can report the same version (e.g. 9.1.0), so
            // compare Git revisions instead. For all-old replica sets, use the latest shell's
            // build information as the reference.
            const latestNodeIndex = TestData.mixedBinVersions.indexOf("new");
            const latestBuildInfo =
                latestNodeIndex === -1
                    ? buildInfo()
                    : assert.commandWorked(
                          new Mongo(members[latestNodeIndex]["name"]).adminCommand({buildInfo: 1}),
                      );
            assert(latestBuildInfo.gitVersion, "Latest build has no Git revision", {
                latestBuildInfo,
            });
            const actualBuildInfo = assert.commandWorked(admin.runCommand({buildInfo: 1}));
            assert.neq(
                latestBuildInfo.gitVersion,
                actualBuildInfo.gitVersion,
                "last-patch node unexpectedly running the latest Git revision",
                {
                    actualVersion,
                    latestBinVersion,
                    actualGitVersion: actualBuildInfo.gitVersion,
                    latestGitVersion: latestBuildInfo.gitVersion,
                },
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
