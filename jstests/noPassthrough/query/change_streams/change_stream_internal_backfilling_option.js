/**
 * Tests that the internal-only '$changeStream.backfillingStartAtOperationTime' option cannot be set
 * by an external client. A client that self-declares as an internal component through the
 * 'internalClient' hello field still cannot use it, and 'fromRouter' is a request field that such a
 * client cannot legitimately set.
 *
 * @tags: [
 *   requires_auth,
 *   requires_replication,
 *   uses_change_streams,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("internal-only change stream backfilling option", function () {
    const dbName = jsTestName();
    const collName = "coll";
    const password = "pwd";

    let rst;
    let primary;
    let savedSkipGossipingClusterTime;

    before(function () {
        // A connection that self-declares as internal receives an unsigned $clusterTime from the
        // server. Once that value is cached, the shell gossips it back on the next command, and the
        // server rejects it with KeyNotFound because the client is not actually authorized to
        // advance the clock. Disable cluster time gossip so the commands below reach the
        // $changeStream option validation instead of failing in request metadata processing.
        savedSkipGossipingClusterTime = TestData.skipGossipingClusterTime;
        TestData.skipGossipingClusterTime = true;

        rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1"});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();

        const adminDB = primary.getDB("admin");
        assert.commandWorked(
            adminDB.runCommand({
                createUser: "admin",
                pwd: password,
                roles: ["root"],
                writeConcern: {w: "majority"},
            }),
        );
        assert(adminDB.auth("admin", password));

        assert.commandWorked(
            primary.getDB(dbName).runCommand({
                createUser: "user",
                pwd: password,
                roles: [{role: "readWrite", db: dbName}],
            }),
        );
        assert.commandWorked(primary.getDB(dbName)[collName].insert({_id: 0}));
    });

    after(function () {
        TestData.skipGossipingClusterTime = savedSkipGossipingClusterTime;
        if (rst) {
            rst.stopSet();
        }
    });

    // Returns a fresh connection authenticated as the ordinary user.
    function externalConnection() {
        const conn = new Mongo(primary.host);
        assert(conn.getDB(dbName).auth("user", password), "Authentication failed");
        return conn;
    }

    function withExternalConnection(cb) {
        let conn = externalConnection();
        try {
            cb(conn);
        } finally {
            conn.close();
        }
    }

    // Returns a connection authenticated as the ordinary user that has additionally claimed to be
    // an internal component via the 'internalClient' hello field.
    function selfDeclaredInternalConnection() {
        const conn = externalConnection();
        const hello = assert.commandWorked(conn.adminCommand({hello: 1}));
        assert.commandWorked(
            conn.adminCommand({
                hello: 1,
                internalClient: {minWireVersion: 0, maxWireVersion: hello.maxWireVersion},
            }),
        );
        return conn;
    }

    function withSelfDeclaredInternalConnection(cb) {
        let conn = selfDeclaredInternalConnection();
        try {
            cb(conn);
        } finally {
            conn.close();
        }
    }

    // An aggregate that carries the internal-only backfilling option, plus an explicit writeConcern
    // because a connection that claims to be internal does not get the default write concern.
    function backfillingAggregate(extraFields = {}) {
        return Object.assign(
            {
                aggregate: collName,
                pipeline: [
                    {
                        $changeStream: {
                            startAtOperationTime: Timestamp(100, 1),
                            backfillingStartAtOperationTime: Timestamp(90, 1),
                        },
                    },
                ],
                cursor: {},
                writeConcern: {},
            },
            extraFields,
        );
    }

    it("allows an ordinary change stream from the external client", function () {
        withExternalConnection((conn) => {
            const res = assert.commandWorked(
                conn.getDB(dbName).runCommand({
                    aggregate: collName,
                    pipeline: [{$changeStream: {}}],
                    cursor: {},
                }),
            );
            assert.commandWorked(
                conn.getDB(dbName).runCommand({
                    killCursors: collName,
                    cursors: [res.cursor.id],
                }),
            );
        });
    });

    it("rejects the option from the external client", function () {
        withExternalConnection((conn) => {
            assert.commandFailedWithCode(
                conn.getDB(dbName).runCommand(backfillingAggregate()),
                ErrorCodes.InvalidOptions,
            );
        });
    });

    it("rejects the option from the external client even when it claims to be internal", function () {
        withSelfDeclaredInternalConnection((conn) => {
            assert.commandFailedWithCode(
                conn.getDB(dbName).runCommand(backfillingAggregate()),
                ErrorCodes.InvalidOptions,
            );
        });
    });

    it("rejects 'fromRouter' from the external client even when it claims to be internal", function () {
        withSelfDeclaredInternalConnection((conn) => {
            assert.commandFailedWithCode(
                conn
                    .getDB(dbName)
                    .runCommand(backfillingAggregate({fromRouter: true, readConcern: {}})),
                ErrorCodes.BadValue,
            );
        });
    });
});
