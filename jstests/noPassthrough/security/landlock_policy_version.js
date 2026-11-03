/**
 * Verifies the Landlock policy versioning scheme (security.landlock.policyVersion):
 *  - a server with no configured version applies the latest policy it implements, and
 *    advertises both versions through the "landlock" serverStatus section;
 *  - a version this binary does not implement, or that is not a valid version at all, is refused
 *    at startup, in every sandbox mode;
 *  - security.landlock.additionalPathRules requires policyVersion to be set, and each entry that
 *    is well formed and names an existing path grants exactly the rights it names.
 *
 * Whether a denied operation actually fails under the sandbox is covered by landlock_denial.js;
 * this test is about the option contract. Nodes run "bestEffort" (the shipped default is
 * "disabled"), and the grant assertions apply only where this kernel enforces Landlock. The
 * grants are read from the node's log file, since the sandbox exposes no other record of them.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertLandlockMode,
    useLandlockProcessFixture,
    getLandlockRootRelativeScratchDirectory,
    logEntriesWithId,
    withProcess,
    probeLandlockSupport,
} from "jstests/noPassthrough/security/landlock_helpers.js";

if (getBuildInfo().buildEnvironment.target_os !== "linux") {
    jsTest.log.info("Landlock is Linux-only; skipping test");
    quit();
}

const kernel = probeLandlockSupport();

function assertRefused({start}, opts) {
    assert.throws(() => start(opts), [], "node started despite invalid options", {opts});
}

describe("security.landlock.policyVersion", function () {
    const fixture = useLandlockProcessFixture();

    for (const [name, process] of Object.entries(fixture.processes)) {
        describe(name, function () {
            it("default version is always the latest", function () {
                withProcess(process, {landlockMode: "disabled"}, (conn) => {
                    const landlock = assertLandlockMode(conn, "disabled");
                    assert.eq(landlock.policyVersion, kernel.latestPolicyVersion);
                    assert.eq(landlock.latestPolicyVersion, kernel.latestPolicyVersion);
                });
            });

            it("accepts the latest version", function () {
                withProcess(
                    process,
                    {landlockMode: "bestEffort", landlockPolicyVersion: kernel.latestPolicyVersion},
                    (conn) => {
                        const landlock = assertLandlockMode(conn, "bestEffort");
                        assert.eq(landlock.policyVersion, kernel.latestPolicyVersion);
                        assert.eq(landlock.latestPolicyVersion, kernel.latestPolicyVersion);
                    },
                );
            });

            it("refuses a version below 1", function () {
                assertRefused(process, {landlockMode: "bestEffort", landlockPolicyVersion: 0});
                assertRefused(process, {landlockMode: "bestEffort", landlockPolicyVersion: -1});
            });

            it("refuses a version newer than what the server implements", function () {
                assertRefused(process, {landlockPolicyVersion: kernel.latestPolicyVersion + 1});
            });

            it(`refuses a non-integer version`, function () {
                assertRefused(process, {landlockPolicyVersion: 1.2});
            });
        });
    }
});

describe("security.landlock.additionalPathRules", function () {
    const fixture = useLandlockProcessFixture();

    const root = getLandlockRootRelativeScratchDirectory("rules");
    const grantedDir = `${root}/granted`;

    before(function () {
        mkdir(root);
        mkdir(grantedDir);
    });

    after(function () {
        removeFile(root);
    });

    for (const [name, process] of Object.entries(fixture.processes)) {
        describe(name, function () {
            const kRule = `r:${root}`;

            it("requires an explicit policyVersion", function () {
                // The requirement does not depend on the sandbox being enabled.
                assertRefused(process, {
                    landlockMode: "disabled",
                    landlockAdditionalPathRules: kRule,
                });
            });

            it("rejects a malformed access list", function () {
                const badRules = [`bogus:${root}`, `r,:${root}`, `read_file,,write_file:${root}`];
                for (const rule of badRules) {
                    assertRefused(process, {
                        landlockPolicyVersion: 1,
                        landlockAdditionalPathRules: rule,
                    });
                }
            });

            it("refuses a path that does not exist", function () {
                if (!kernel.active) {
                    jsTest.log.info("kernel does not enforce Landlock; skipping test");
                    return;
                }
                assertRefused(process, {
                    landlockMode: "bestEffort",
                    landlockPolicyVersion: 1,
                    landlockAdditionalPathRules: `r:${root}/missing`,
                });
            });

            it("applies a valid additionalPathRule at the given policyVersion", function () {
                if (!kernel.active) {
                    jsTest.log.info("kernel does not enforce Landlock; skipping test");
                    return;
                }
                withProcess(
                    process,
                    {
                        landlockMode: "bestEffort",
                        landlockPolicyVersion: 1,
                        landlockAdditionalPathRules: `r,Write_File,execute:${grantedDir}`,
                        useLogFiles: true,
                    },
                    (conn) => {
                        const landlock = assertLandlockMode(conn, "bestEffort");
                        assert.eq(1, landlock.policyVersion);

                        // one path rule granted (attr.path, attr.allowedAccess)
                        const kIdRuleApplied = 13118805;
                        // the configured additionalPathRules, before they apply
                        const kIdAdditionalRules = 13214200;

                        const [configured] = logEntriesWithId(conn, kIdAdditionalRules);
                        assert.neq(undefined, configured, "additionalPathRules were not logged");

                        const rule = logEntriesWithId(conn, kIdRuleApplied).find(
                            (e) => e.attr.path === grantedDir,
                        );
                        assert.neq(undefined, rule, "no rule was applied for the configured path", {
                            grantedDir,
                        });

                        const kFsPrefix = "LANDLOCK_ACCESS_FS_";
                        const granted = rule.attr.allowedAccess.map((name) =>
                            name.replace(kFsPrefix, ""),
                        );
                        assert.sameMembers(
                            ["READ_FILE", "READ_DIR", "WRITE_FILE", "EXECUTE"],
                            granted,
                            "wrong grants",
                            undefined,
                            {rule},
                        );
                    },
                );
            });
        });
    }
});
