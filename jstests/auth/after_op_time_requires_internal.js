/**
 * Tests that readConcern.afterOpTime is only accepted from clients that hold ActionType::internal,
 * on both mongod and mongos, and that the same commands without afterOpTime are unaffected.
 *
 * The authorization failure is rewrapped as "not authorized on <db> to execute command <cmd>", so
 * rather than matching the error message, each rejected command is paired with a control that runs
 * the same command without afterOpTime and must succeed for the same user.
 *
 * The 'allowDeprecatedAfterOpTimeReadConcern' server parameter drops the restriction entirely.
 * Those tests flip it on at runtime, check that the commands that were rejected are then accepted,
 * and flip it back off.
 *
 * @tags: [requires_sharding]
 */

const kPwd = "pwd";
const kDbName = "after_op_time_requires_internal";
const kCollName = "coll";
const kRootUser = "rootUser";
const kInternalUser = "internalUser";

const withTopLevelReadConcern = (cmd, readConcern) => Object.assign({}, cmd, {readConcern});

// 'ping' and 'hello' need no privileges of their own. 'withReadConcern' builds the variant of 'cmd'
// that carries the read concern; by default it is a top-level generic argument.
const kCommands = [
    {name: "find", dbName: kDbName, cmd: {find: kCollName, filter: {}}},
    {name: "aggregate", dbName: kDbName, cmd: {aggregate: kCollName, pipeline: [], cursor: {}}},
    {
        name: "insert",
        dbName: kDbName,
        cmd: {insert: kCollName, documents: [{_id: "afterOpTimeProbe"}]},
        // The rejected variant never executes, so it can reuse the control's document. When the
        // argument is accepted the insert does execute, so it needs an _id of its own.
        cmdIfAccepted: (cmd, readConcern) =>
            Object.assign({}, cmd, {documents: [{_id: "afterOpTimeProbeAccepted"}], readConcern}),
    },
    {name: "ping", dbName: "admin", cmd: {ping: 1}},
    {name: "hello", dbName: "admin", cmd: {hello: 1}},
    {
        name: "explain with outer readConcern",
        dbName: kDbName,
        cmd: {explain: {find: kCollName, filter: {}}, verbosity: "queryPlanner"},
    },
    {
        // The nested command is authorized recursively, so its own readConcern must be checked.
        name: "explain with inner readConcern",
        dbName: kDbName,
        cmd: {explain: {find: kCollName, filter: {}}, verbosity: "queryPlanner"},
        withReadConcern: (cmd, readConcern) =>
            Object.assign({}, cmd, {explain: Object.assign({}, cmd.explain, {readConcern})}),
    },
];

function createUsers(adminDB) {
    assert.commandWorked(adminDB.runCommand({createUser: kRootUser, pwd: kPwd, roles: ["root"]}));
    assert(adminDB.auth(kRootUser, kPwd));
    assert.commandWorked(
        adminDB.runCommand({createUser: kInternalUser, pwd: kPwd, roles: ["__system"]}));
    adminDB.logout();
}

function connectAs(host, user) {
    const conn = new Mongo(host);
    if (user) {
        assert(conn.getDB("admin").auth(user, kPwd), `failed to authenticate as ${user}`);
    }
    return conn;
}

function assertAfterOpTimeRejected(conn, afterOpTime) {
    for (const {name, dbName, cmd, withReadConcern = withTopLevelReadConcern} of kCommands) {
        const testDB = conn.getDB(dbName);

        assert.commandWorked(testDB.runCommand(cmd), `control failed for ${name}`);
        assert.commandFailedWithCode(testDB.runCommand(withReadConcern(cmd, {afterOpTime})),
                                     ErrorCodes.Unauthorized,
                                     `afterOpTime should have been rejected for ${name}`);
    }
}

function assertUnauthenticatedPingRejected(host, afterOpTime) {
    const adminDB = connectAs(host).getDB("admin");
    assert.commandWorked(adminDB.runCommand({ping: 1}));
    assert.commandFailedWithCode(adminDB.runCommand({ping: 1, readConcern: {afterOpTime}}),
                                 ErrorCodes.Unauthorized);
}

// When the 'allowDeprecatedAfterOpTimeReadConcern' escape hatch is on, every client is treated as
// if it held ActionType::internal, so the commands that 'assertAfterOpTimeRejected' rejects must
// now succeed. There is no control to compare against here: the failure of the same command
// without afterOpTime is already covered above.
function assertAfterOpTimeAccepted(conn, afterOpTime) {
    for (const {name, dbName, cmd, cmdIfAccepted = withTopLevelReadConcern} of kCommands) {
        assert.commandWorked(conn.getDB(dbName).runCommand(cmdIfAccepted(cmd, {afterOpTime})),
                             `afterOpTime should have been accepted for ${name}`);
    }
}

// Flips the escape hatch. It is settable at runtime, so the tests below enable it, check the
// behavior it is meant to change, and then disable it again.
function setAllowDeprecatedAfterOpTimeReadConcern(conn, enabled) {
    assert.commandWorked(conn.getDB("admin").runCommand({
        setParameter: 1,
        allowDeprecatedAfterOpTimeReadConcern: enabled,
    }));
}

function getAfterOpTimeCounters(conn) {
    // Don't return defaultRWConcern: asking for it can refresh the read write concern defaults,
    // which runs commands of its own.
    const status = assert.commandWorked(
        conn.getDB("admin").runCommand({serverStatus: 1, defaultRWConcern: false}));
    const counters = status.readConcernCounters && status.readConcernCounters.afterOpTime;
    // This shell's assert() takes no attributes argument, so the context goes in the message.
    assert(counters,
           "Missing 'readConcernCounters.afterOpTime' from serverStatus: " +
               tojson(status.readConcernCounters));
    return counters;
}

{
    jsTest.log("readConcern.afterOpTime on mongod");
    const rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1"});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    createUsers(primary.getDB("admin"));

    // Seed the collection and capture an optime that is already satisfied.
    const internalConn = connectAs(primary.host, kInternalUser);
    assert.commandWorked(internalConn.getDB(kDbName)[kCollName].insert({_id: 0}));
    const lastOp = internalConn.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next();
    const afterOpTime = {ts: lastOp.ts, t: lastOp.t};

    jsTest.log("Rejected for an authenticated user without internal");
    assertAfterOpTimeRejected(connectAs(primary.host, kRootUser), afterOpTime);

    jsTest.log("Rejected for an unauthenticated connection, even for commands needing no auth");
    assertUnauthenticatedPingRejected(primary.host, afterOpTime);

    jsTest.log("Accepted for a user with internal");
    for (const {name, dbName, cmd} of kCommands.filter(({name}) => name === "find" ||
                                                           name === "ping")) {
        assert.commandWorked(
            internalConn.getDB(dbName).runCommand(withTopLevelReadConcern(cmd, {afterOpTime})),
            `afterOpTime should be accepted for ${name}`);
    }

    jsTest.log("With allowDeprecatedAfterOpTimeReadConcern enabled");
    {
        // Only an authenticated connection can change a server parameter: the framework's
        // connection to the primary is not authenticated, it created the users through the
        // localhost exception and then logged out.
        const rootConn = connectAs(primary.host, kRootUser);
        setAllowDeprecatedAfterOpTimeReadConcern(rootConn, true);

        jsTest.log("Accepted for an authenticated user without internal");
        assertAfterOpTimeAccepted(rootConn, afterOpTime);

        jsTest.log("Accepted for an unauthenticated connection, even for commands needing no auth");
        assert.commandWorked(connectAs(primary.host).getDB("admin").runCommand({
            ping: 1,
            readConcern: {afterOpTime}
        }));

        jsTest.log("Counted as accepted rather than rejected");
        const before = getAfterOpTimeCounters(internalConn);
        assert.commandWorked(rootConn.getDB(kDbName).runCommand(
            {find: kCollName, filter: {}, readConcern: {afterOpTime}}));
        const after = getAfterOpTimeCounters(internalConn);
        // Other operations can also carry an afterOpTime, so 'accepted' is only bounded from
        // below; 'rejected' must not move at all.
        assert.gte(after.accepted,
                   before.accepted + 1,
                   "afterOpTime was not counted as accepted: " + tojson({before, after}));
        assert.eq(after.rejected,
                  before.rejected,
                  "afterOpTime was counted as rejected: " + tojson({before, after}));

        jsTest.log("Rejected again once the parameter is disabled");
        setAllowDeprecatedAfterOpTimeReadConcern(rootConn, false);
        // Only one command here: 'assertAfterOpTimeRejected' would run its insert control a
        // second time, and the document it writes is already there from the test above.
        assert.commandFailedWithCode(
            connectAs(primary.host, kRootUser)
                .getDB(kDbName)
                .runCommand({find: kCollName, filter: {}, readConcern: {afterOpTime}}),
            ErrorCodes.Unauthorized);
    }

    rst.stopSet();
}

{
    jsTest.log("readConcern.afterOpTime on mongos");
    // Its value only needs to be well-formed: the router rejects the request before forwarding it.
    const kOpTimeForRouter = {ts: Timestamp(1, 1), t: NumberLong(1)};

    const st = new ShardingTest({shards: 1, config: 1, other: {keyFile: "jstests/libs/key1"}});
    createUsers(st.s.getDB("admin"));

    jsTest.log("Rejected for an authenticated user without internal");
    assertAfterOpTimeRejected(connectAs(st.s.host, kRootUser), kOpTimeForRouter);

    jsTest.log("Rejected for an unauthenticated connection, even for commands needing no auth");
    assertUnauthenticatedPingRejected(st.s.host, kOpTimeForRouter);

    jsTest.log("With allowDeprecatedAfterOpTimeReadConcern enabled");
    {
        // The router applies the check itself, and then forwards the client's readConcern
        // ('readConcern' is a generic argument that is forwarded to shards) to the shard. Enable
        // the parameter on both rather than rely on how the shard authorizes requests coming from
        // the router. Neither framework connection is authenticated: 'createUsers' logged out of
        // the router, and jsTest.authenticate() is a no-op here since the keyfile is only passed
        // to the ShardingTest. So the router is reached as the root user, and the shard, which
        // does not have the users created through the router, as the cluster.
        const rootConn = connectAs(st.s.host, kRootUser);
        const setOnRouterAndShard = (enabled) => {
            setAllowDeprecatedAfterOpTimeReadConcern(rootConn, enabled);
            authutil.asCluster(st.shard0,
                               "jstests/libs/key1",
                               () => setAllowDeprecatedAfterOpTimeReadConcern(st.shard0, enabled));
        };
        setOnRouterAndShard(true);

        jsTest.log("Accepted for an authenticated user without internal");
        assertAfterOpTimeAccepted(rootConn, kOpTimeForRouter);

        jsTest.log("Accepted for an unauthenticated connection, even for commands needing no auth");
        assert.commandWorked(connectAs(st.s.host).getDB("admin").runCommand(
            {ping: 1, readConcern: {afterOpTime: kOpTimeForRouter}}));

        setOnRouterAndShard(false);
    }

    st.stop();
}
