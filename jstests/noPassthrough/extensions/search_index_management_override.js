/**
 * Tests that the Search Index Management extension overrides the legacy search index management
 * implementations at startup.
 *
 * The createSearchIndexes, updateSearchIndex, and dropSearchIndex commands: the extension's commands
 * are used only when both featureFlagExtensionCommands and
 * featureFlagExtensionSearchIndexManagement are enabled. Otherwise the legacy commands are kept and
 * the extension's registrations of those names are skipped.
 *
 * The $listSearchIndexes stage: the extension's stage is used when
 * featureFlagExtensionSearchIndexManagement is enabled, and the legacy fallback otherwise.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const kSearchIndexManagementExtension = {"libsearch_index_management_extension.so": {}};
const collName = jsTestName();

const kSearchIndexCommands = [
    {
        createSearchIndexes: collName,
        indexes: [{name: "idx", definition: {mappings: {dynamic: true}}}],
    },
    {updateSearchIndex: collName, name: "idx", definition: {mappings: {dynamic: true}}},
    {dropSearchIndex: collName, name: "idx"},
];

/*
 * Runs 'testFn' with extension commands and search index management selected by the two flags. Runs
 * both topologies by default.
 */
function withFlags(
    extensionCommands,
    extensionSearchIndexManagement,
    testFn,
    {topologies = ["standalone", "sharded"], shardingOptions = {}, options = {}} = {},
) {
    withExtensions(kSearchIndexManagementExtension, testFn, topologies, shardingOptions, {
        ...options,
        setParameter: {
            featureFlagExtensionCommands: extensionCommands,
            featureFlagExtensionSearchIndexManagement: extensionSearchIndexManagement,
        },
    });
}

/*
 * The legacy commands require a remote search index management host, which these tests do not
 * configure, so reaching a legacy command is observable as SearchNotEnabled.
 */
function assertLegacyUsed(testDB) {
    for (const cmd of kSearchIndexCommands) {
        assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.SearchNotEnabled, {cmd});
    }
}

/*
 * The mock extension commands succeed and report the target namespace they were given.
 */
function assertExtensionUsed(testDB) {
    for (const cmd of kSearchIndexCommands) {
        const res = assert.commandWorked(testDB.runCommand(cmd), {cmd});
        assert.eq(res.collection, collName, {cmd, res});
    }
}

/*
 * The extension $listSearchIndexes is a stub that acts as a no-op, passing documents through. The
 * legacy $listSearchIndexes throws SearchNotEnabled when search index management is not configured.
 * We can use this behavior to test whether the extension or the legacy implementation is being
 * used.
 */
const pipeline = [{$listSearchIndexes: {}}];

const kEnabledOptions = {setParameter: {featureFlagExtensionSearchIndexManagement: true}};
const kDisabledOptions = {setParameter: {featureFlagExtensionSearchIndexManagement: false}};

/*
 * Reads the current value of the featureFlagExtensionSearchIndexManagement flag.
 */
function readFlagValue(adminDb) {
    return assert.commandWorked(
        adminDb.runCommand({getParameter: 1, featureFlagExtensionSearchIndexManagement: 1}),
    ).featureFlagExtensionSearchIndexManagement.value;
}

/*
 * Asserts the implementation selected by the flag currently in effect, and returns that flag value.
 */
function runListSearchIndexesOverrideTest(conn, {shardingTest = null} = {}) {
    const adminDb = conn.getDB("admin");
    const coll = conn.getDB("test")[jsTestName()];
    coll.drop();
    const testData = [
        {_id: 0, text: "apple"},
        {_id: 1, text: "banana"},
        {_id: 2, text: "cherry"},
    ];
    assert.commandWorked(coll.insertMany(testData));
    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1});
    }

    const featureFlagEnabled = readFlagValue(adminDb);
    if (featureFlagEnabled) {
        // The extension's stub primary parser is selected and passes documents through unchanged.
        assertArrayEq({actual: coll.aggregate(pipeline).toArray(), expected: testData});
    } else {
        // The legacy fallback parser is selected, which fails because search index management is
        // not configured on the node running the stage (mongod standalone, or mongos when sharded).
        assert.throwsWithCode(
            () => coll.aggregate(pipeline).toArray(),
            ErrorCodes.SearchNotEnabled,
        );
    }
    return featureFlagEnabled;
}

/*
 * Verifies that each mock command enforces the privilege it declares, which is the action named
 * after the command.
 */
function runSearchIndexAuthTest(conn) {
    const admin = conn.getDB("admin");
    admin.createUser({user: "adminUser", pwd: "pwd", roles: ["root"]});
    assert(admin.auth("adminUser", "pwd"));

    const testDB = conn.getDB(jsTestName());
    assert.commandWorked(testDB.createCollection(collName));
    for (const cmd of kSearchIndexCommands) {
        const action = Object.keys(cmd)[0];
        testDB.createRole({
            role: `${action}Role`,
            privileges: [
                {
                    resource: {db: testDB.getName(), collection: collName},
                    actions: [action],
                },
            ],
            roles: [],
        });
        testDB.createUser({
            user: `${action}User`,
            pwd: "pwd",
            roles: [`${action}Role`],
        });
    }
    testDB.createUser({user: "noPrivilegesUser", pwd: "pwd", roles: []});
    admin.logout();

    function asUser(user, fn) {
        assert(testDB.auth(user, "pwd"));
        try {
            fn();
        } finally {
            testDB.logout();
        }
    }

    for (const cmd of kSearchIndexCommands) {
        const action = Object.keys(cmd)[0];

        // A user holding the declared action can run the command.
        asUser(`${action}User`, () => {
            assert.commandWorked(testDB.runCommand(cmd), {cmd});
        });

        // A user without it is rejected.
        asUser("noPrivilegesUser", () => {
            assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.Unauthorized, {cmd});
        });
    }
}

describe("search index management extension override", function () {
    describe("search index management commands", function () {
        for (const extensionCommands of [false, true]) {
            for (const searchIndexManagement of [false, true]) {
                const usesExtension = extensionCommands && searchIndexManagement;
                const flagSettings =
                    `featureFlagExtensionCommands=${extensionCommands}, ` +
                    `featureFlagExtensionSearchIndexManagement=${searchIndexManagement}`;
                const selected = usesExtension ? "extension" : "legacy";
                it(`uses the ${selected} commands when ${flagSettings}`, function () {
                    withFlags(extensionCommands, searchIndexManagement, (conn) => {
                        const testDB = conn.getDB(jsTestName());
                        assert.commandWorked(testDB.createCollection(collName));
                        if (usesExtension) {
                            assertExtensionUsed(testDB);
                        } else {
                            assertLegacyUsed(testDB);
                        }
                    });
                });
            }
        }

        // Standalone and sharded need different configuration setups to enable auth, so they run
        // separately.
        it("enforces the declared search index privileges on a standalone", function () {
            withFlags(true, true, runSearchIndexAuthTest, {
                topologies: ["standalone"],
                options: {auth: ""},
            });
        });

        it("enforces the declared search index privileges on a sharded cluster", function () {
            withFlags(true, true, runSearchIndexAuthTest, {
                topologies: ["sharded"],
                shardingOptions: {other: {keyFile: "jstests/libs/key1"}},
            });
        });
    });

    describe("$listSearchIndexes stage", function () {
        it("selects the implementation from featureFlagExtensionSearchIndexManagement", function () {
            const observedFlagValues = [];

            withExtensions(
                kSearchIndexManagementExtension,
                (conn, shardingTest) =>
                    observedFlagValues.push(runListSearchIndexesOverrideTest(conn, {shardingTest})),
                [
                    {topology: "standalone", mongodOptions: kEnabledOptions},
                    {topology: "standalone", mongodOptions: kDisabledOptions},
                    {topology: "sharded", mongodOptions: kEnabledOptions},
                    {topology: "sharded", mongodOptions: kDisabledOptions},
                ],
                // Multi-shard so the extension passthrough is exercised across shards.
                {shards: 2, rs: {nodes: 2}},
            );

            assert.eq(
                observedFlagValues,
                [true, false, true, false],
                "each run should have observed the flag value it was started with",
            );
        });
    });
});
