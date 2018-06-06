// This tests the fix for SERVER-33553. It creates a situation where a cursor created as part of a
// session is killed using a legacy OP_KILL_CURSORS request. This is not "normal" behavior that's
// expected from a driver, so there is a certain amount of set up work that needs to happen.
(function() {
    "use strict";

    const st = new ShardingTest({shards: 1, verbose: 2, keyFile: "jstests/libs/key1"});

    const collName = "legacy_kill_op_test";

    const adminDB = st.s.getDB("admin");
    adminDB.createUser({user: 'user', pwd: 'user', roles: jsTest.adminUserRoles});
    adminDB.auth('user', 'user');

    const coll = st.s.getDB("test")[collName];

    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({a: 2}));
    assert.writeOK(coll.insert({a: 3}));

    const session = st.s.startSession();
    const sessionDB = session.getDatabase("test");
    const cmdRes = sessionDB.runCommand({find: collName, batchSize: 2});
    assert.commandWorked(cmdRes);

    function killCursorInLegacyMode() {
        db.getSiblingDB("admin").auth("user", "user");
        db.getMongo().forceWriteMode("legacy");

        const curs = new DBCommandCursor(db, cmdRes);

        // Calling close() should send an OP_KILL_CURSORS request.
        curs.close();

        db.getMongo().forceWriteMode("commands");

        // Be sure that we cannot kill the cursor, to prove that curs.close() killed it
        // successfully. The cursor may be in a "zombie" state for some time before it is actually
        // killed, so we need the assert.soon.
        assert.soon(function() {
            const matchingCursors = db.getSiblingDB("admin")
                                        .aggregate([
                                            {"$listLocalCursors": {}},
                                            {"$match": {"id": cmdRes.cursor.id}},
                                        ])
                                        .toArray();
            return matchingCursors.length === 0;
        });
    }
    let code = `const cmdRes = ${tojson(cmdRes)};`;
    code += `const collName = "${collName}";`;
    code += `(${killCursorInLegacyMode.toString()})();`;

    const joinFn = startParallelShell(code, st.s.port);
    joinFn();
    st.stop();
})();
