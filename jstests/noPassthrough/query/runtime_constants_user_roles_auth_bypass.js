/**
 * Tests that an external client cannot set 'runtimeConstants.userRoles', which the server derives
 * from the authenticated user, while other runtime constants and internal clients are unaffected.
 *
 * @tags: [
 *   requires_auth,
 *   requires_fcv_70,
 * ]
 */

// Thrown by Variables::validateRuntimeConstantsArePermitted() for a supplied 'userRoles'.
const kUserRolesNotPermitted = 12843300;
// Thrown by aggregate when 'runtimeConstants' is set without 'fromMongos'.
const kAggregateRuntimeConstants = 463840;

// Matches documents when the authenticated user holds the named role.
const rolePredicate = (role) => ({
    $expr: {
        $in: [role, {$map: {input: "$$USER_ROLES", as: "roleDoc", in: "$$roleDoc.role"}}],
    },
});

// Matches only if the effective $$USER_ROLES contain "root", which the test users never
// legitimately hold.
const rootRolePredicate = rolePredicate("root");

const seedDocs = [
    {_id: 1, v: 0},
    {_id: 2, v: 0},
];

const readWriteUserCommand = (user, password, dbName) => ({
    createUser: user,
    pwd: password,
    roles: [{role: "readWrite", db: dbName}],
});

const elevatedRoleRuntimeConstants = () => ({
    localNow: new Date(),
    clusterTime: Timestamp(1, 1),
    userRoles: [{_id: "admin.root", role: "root", db: "admin"}],
});

const benignRuntimeConstants = () => ({
    localNow: new Date(),
    clusterTime: Timestamp(1, 1),
});

const withRuntimeConstants = (command, constants = elevatedRoleRuntimeConstants()) => ({
    ...command,
    runtimeConstants: constants,
});

const explainWithRuntimeConstants = (command, constants) => ({
    explain: withRuntimeConstants(command, constants),
});

const findCase = {
    name: "find",
    command: (collName, constants, filter = rootRolePredicate) =>
        withRuntimeConstants({find: collName, filter}, constants),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: 51202,
};

const findExplainCase = {
    name: "find explain",
    command: (collName, constants, filter = rootRolePredicate) =>
        explainWithRuntimeConstants({find: collName, filter}, constants),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: 51202,
};

const updateQueryPredicatesCase = {
    name: "update query predicates",
    command: (collName, constants, q = rootRolePredicate) =>
        withRuntimeConstants(
            {
                update: collName,
                updates: [{q, u: {$set: {v: 9}}, multi: true}],
            },
            constants,
        ),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: 51195,
    internalClientFields: {writeConcern: {}},
};

const updatePipelineExpressionsCase = {
    name: "update pipeline expressions",
    command: (collName, constants, q = {_id: 1}) =>
        withRuntimeConstants(
            {
                update: collName,
                updates: [{q, u: [{$set: {injectedRoles: "$$USER_ROLES.role"}}]}],
            },
            constants,
        ),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: 51195,
    internalClientFields: {writeConcern: {}},
};

const updateExplainCase = {
    name: "update explain",
    command: (collName, constants, q = rootRolePredicate) =>
        explainWithRuntimeConstants(
            {
                update: collName,
                updates: [{q, u: {$set: {v: 9}}, multi: true}],
            },
            constants,
        ),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: 51195,
};

const deleteCase = {
    name: "delete",
    command: (collName, constants, q = rootRolePredicate) =>
        withRuntimeConstants(
            {
                delete: collName,
                deletes: [{q, limit: 0}],
            },
            constants,
        ),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: kUserRolesNotPermitted,
    internalClientFields: {writeConcern: {}},
};

const deleteExplainCase = {
    name: "delete explain",
    command: (collName, constants, q = rootRolePredicate) =>
        explainWithRuntimeConstants(
            {
                delete: collName,
                deletes: [{q, limit: 0}],
            },
            constants,
        ),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: kUserRolesNotPermitted,
};

const findAndModifyUpdateCase = {
    name: "findAndModify update",
    command: (collName, constants, query = rootRolePredicate) =>
        withRuntimeConstants(
            {
                findAndModify: collName,
                query,
                update: {$set: {v: 9}},
            },
            constants,
        ),
    mongodCode: kUserRolesNotPermitted,
    // The router rejects findAndModify in run() rather than at construction, so the code
    // depends on which write path is active: the unified write executor throws 11423300 while
    // the legacy cluster findAndModify path throws 51196. Accept either.
    mongosCode: [51196, 11423300],
    internalClientFields: {writeConcern: {}},
};

const findAndModifyDeleteCase = {
    name: "findAndModify delete",
    command: (collName, constants, query = rootRolePredicate) =>
        withRuntimeConstants(
            {
                findAndModify: collName,
                query,
                remove: true,
            },
            constants,
        ),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: [51196, 11423300],
    internalClientFields: {writeConcern: {}},
};

const findAndModifyExplainCase = {
    name: "findAndModify explain",
    command: (collName, constants, query = rootRolePredicate) =>
        explainWithRuntimeConstants(
            {
                findAndModify: collName,
                query,
                update: {$set: {v: 9}},
            },
            constants,
        ),
    mongodCode: kUserRolesNotPermitted,
    mongosCode: [51196, 11423300],
};

const aggregateCase = {
    name: "aggregate",
    command: (collName, constants) =>
        withRuntimeConstants(
            {
                aggregate: collName,
                pipeline: [{$project: {_id: 0, roles: "$$USER_ROLES"}}],
                cursor: {},
            },
            constants,
        ),
    mongodCode: kAggregateRuntimeConstants,
    mongosCode: 51143,
    // Aggregate only accepts 'runtimeConstants' together with these fields.
    internalClientFields: {fromMongos: true, readConcern: {}, writeConcern: {}},
};

const testCases = [
    findCase,
    findExplainCase,
    updateQueryPredicatesCase,
    updatePipelineExpressionsCase,
    updateExplainCase,
    deleteCase,
    deleteExplainCase,
    findAndModifyUpdateCase,
    findAndModifyDeleteCase,
    findAndModifyExplainCase,
    aggregateCase,
];

{
    jsTest.log("Suite: runtimeConstants.userRoles handling for an external client (mongod)");

    const dbName = jsTestName();
    const collName = "docs";
    const mergeCollName = "merged_docs";
    const attackerUser = "attacker";
    const attackerPassword = "attackpass";

    const conn = MongoRunner.runMongod({auth: ""});
    assert.neq(null, conn, "mongod failed to start");

    try {
        const adminDB = conn.getDB("admin");
        assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "password", roles: ["root"]}));
        assert(adminDB.auth("admin", "password"), "admin auth failed");

        const attackerDB = conn.getDB(dbName);
        assert.commandWorked(attackerDB.runCommand(readWriteUserCommand(attackerUser, attackerPassword, dbName)));

        adminDB.logout();
        assert(attackerDB.auth(attackerUser, attackerPassword), "attacker auth failed");
        const mergeColl = attackerDB.getCollection(mergeCollName);

        assert.commandWorked(attackerDB.getCollection(collName).insertMany(seedDocs));

        jsTest.log("Test: uses the authenticated user's real roles when runtimeConstants is absent");
        {
            const noMatch = assert.commandWorked(attackerDB.runCommand({find: collName, filter: rootRolePredicate}));
            assert.eq([], noMatch.cursor.firstBatch, "root predicate should not match for a readWrite user");

            const match = assert.commandWorked(
                attackerDB.runCommand({find: collName, filter: rolePredicate("readWrite"), sort: {_id: 1}}),
            );
            assert.eq(
                seedDocs,
                match.cursor.firstBatch,
                "readWrite predicate should match every document for a readWrite user",
            );
        }

        for (const testCase of testCases) {
            jsTest.log(`Test: rejects a supplied runtimeConstants.userRoles for ${testCase.name}`);
            assert.commandFailedWithCode(attackerDB.runCommand(testCase.command(collName)), testCase.mongodCode);
        }

        // Aggregate rejects any external runtimeConstants, see test below.
        for (const testCase of testCases.filter((testCase) => testCase !== aggregateCase)) {
            jsTest.log(`Test: accepts runtimeConstants without userRoles for ${testCase.name}`);
            const command = testCase.command(collName, benignRuntimeConstants(), {_id: -1});
            assert.commandWorked(attackerDB.runCommand(command));
        }

        // Aggregate is stricter than the other commands: it rejects any external
        // 'runtimeConstants', even without 'userRoles'.
        jsTest.log("Test: rejects runtimeConstants without userRoles for aggregate");
        assert.commandFailedWithCode(
            attackerDB.runCommand(aggregateCase.command(collName, benignRuntimeConstants())),
            kAggregateRuntimeConstants,
        );

        const userRolesMergeCommand = {
            aggregate: collName,
            pipeline: [
                {$addFields: {rolesSeenByPipeline: "$$USER_ROLES.role"}},
                {
                    $merge: {
                        into: mergeCollName,
                        on: "_id",
                        whenMatched: "replace",
                        whenNotMatched: "insert",
                    },
                },
            ],
            cursor: {},
        };

        jsTest.log("Test: allows $merge pipelines to propagate derived $$USER_ROLES internally");
        {
            // $merge with whenMatched forwards the derived $$USER_ROLES through runtimeConstants
            // before reparsing the update. This must keep working.
            assert.commandWorked(
                mergeColl.insertMany([
                    {_id: 1, stale: true},
                    {_id: 2, stale: true},
                ]),
            );

            assert.commandWorked(attackerDB.runCommand(userRolesMergeCommand));
            assert.eq(
                [
                    {_id: 1, rolesSeenByPipeline: ["readWrite"]},
                    {_id: 2, rolesSeenByPipeline: ["readWrite"]},
                ],
                mergeColl.find({}, {_id: 1, rolesSeenByPipeline: 1}).sort({_id: 1}).toArray(),
            );
            assert.commandWorked(mergeColl.remove({}));
        }

        jsTest.log("Test: rejects a supplied runtimeConstants.userRoles for $merge pipelines");
        {
            assert.commandFailedWithCode(
                attackerDB.runCommand(withRuntimeConstants(userRolesMergeCommand)),
                kAggregateRuntimeConstants,
            );
            assert.eq([], mergeColl.find().toArray());
        }
    } finally {
        MongoRunner.stopMongod(conn);
    }
}

{
    jsTest.log("Suite: runtimeConstants.userRoles is allowed for an internal client (mongod)");

    const dbName = jsTestName() + "_internal";
    const collName = "docs";

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod failed to start");

    try {
        const internalConn = new Mongo(conn.host);
        assert.commandWorked(
            internalConn.getDB("admin").runCommand({
                hello: 1,
                internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
            }),
        );
        const internalDB = internalConn.getDB(dbName);

        // The commands in this test (update & delete) are actually accepted and will execute.
        // To ensure each test runs against the expected documents and produces observable effects,
        // re-seed the collection before each case so that commands do not no-op on an empty
        // collection.
        const reseed = function () {
            assert.commandWorked(internalDB.runCommand({dropDatabase: 1, writeConcern: {}}));
            assert.commandWorked(
                internalDB.runCommand({
                    insert: collName,
                    documents: seedDocs,
                    writeConcern: {},
                }),
            );
        };

        for (const testCase of testCases) {
            jsTest.log(`Test: accepts runtimeConstants.userRoles for ${testCase.name}`);
            reseed();
            const command = {...testCase.command(collName), ...testCase.internalClientFields};
            assert.commandWorked(internalDB.runCommand(command));
        }

        // The supplied 'userRoles' from the internal client must actually take effect.
        jsTest.log("Test: evaluates $$USER_ROLES from the supplied userRoles in update pipeline expressions");
        {
            reseed();
            assert.commandWorked(
                internalDB.runCommand({
                    ...updatePipelineExpressionsCase.command(collName),
                    ...updatePipelineExpressionsCase.internalClientFields,
                }),
            );

            // This proves the supplied roles are actually applied, not just accepted.
            // Otherwise, deriving $$USER_ROLES from the current user would yield [], not ["root"].
            assert.eq(
                [{_id: 1, v: 0, injectedRoles: ["root"]}],
                internalDB.getCollection(collName).find({_id: 1}).toArray(),
            );
        }
    } finally {
        MongoRunner.stopMongod(conn);
    }
}

{
    jsTest.log("Suite: client-supplied runtimeConstants through a mongos");

    const dbName = jsTestName() + "_sharded";
    const collName = "docs";

    const st = new ShardingTest({shards: 1});

    try {
        const testDB = st.s.getDB(dbName);
        assert.commandWorked(testDB.getCollection(collName).insertMany(seedDocs));

        for (const testCase of testCases) {
            jsTest.log(`Test: rejects runtimeConstants for ${testCase.name}`);
            assert.commandFailedWithCode(testDB.runCommand(testCase.command(collName)), testCase.mongosCode);
        }

        // Unlike find, update, findAndModify, and aggregate, the router does not ban runtime
        // constants on delete outright; it only rejects a client-supplied 'userRoles'.
        jsTest.log("Test: accepts runtimeConstants without userRoles for delete");
        assert.commandWorked(testDB.runCommand(deleteCase.command(collName, benignRuntimeConstants(), {_id: -1})));
    } finally {
        st.stop();
    }
}

{
    jsTest.log("Suite: legitimate $$USER_ROLES predicate end-to-end through mongos");

    const dbName = jsTestName() + "_sharded_auth";
    const collName = "docs";
    const attackerUser = "attacker";
    const attackerPassword = "attackpass";

    // The keyFile enables cluster auth so that $$USER_ROLES reflects a real authenticated user.
    const st = new ShardingTest({
        shards: 1,
        keyFile: "jstests/libs/key1",
    });

    try {
        const adminDB = st.s.getDB("admin");
        assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "password", roles: ["root"]}));
        assert(adminDB.auth("admin", "password"), "admin auth failed");

        const attackerDB = st.s.getDB(dbName);
        assert.commandWorked(attackerDB.runCommand(readWriteUserCommand(attackerUser, attackerPassword, dbName)));

        assert.commandWorked(attackerDB.getCollection(collName).insertMany(seedDocs));

        adminDB.logout();
        assert(attackerDB.auth(attackerUser, attackerPassword), "attacker auth failed");

        jsTest.log("Test: evaluates $$USER_ROLES against the user's real roles when routed through mongos");
        {
            const noMatch = assert.commandWorked(
                attackerDB.runCommand({find: collName, filter: rolePredicate("root")}),
            );
            assert.eq([], noMatch.cursor.firstBatch, "root predicate should not match for a readWrite user");

            const match = assert.commandWorked(
                attackerDB.runCommand({find: collName, filter: rolePredicate("readWrite"), sort: {_id: 1}}),
            );
            assert.eq(
                seedDocs,
                match.cursor.firstBatch,
                "readWrite predicate should match every document for a readWrite user",
            );
        }
    } finally {
        st.stop();
    }
}
