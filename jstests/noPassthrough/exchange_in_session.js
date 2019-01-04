/**
 * Be sure that an exchange won't deadlock when one of the consumer's buffers is full. Iterates two
 * consumers on an Exchange with a very small buffer. This test was designed to reproduce
 * SERVER-37499.
 * @tags: [requires_sharding, uses_transactions]
 */
(function() {
    // This test manually simulates a session, which is not compatible with implicit sessions.
    TestData.disableImplicitSessions = true;

    // Start a sharded cluster. For this test, we'll just need to talk to the shard directly.
    const st = new ShardingTest({shards: 1, mongos: 1});

    const adminDB = st.shard0.getDB("admin");
    const session = st.shard0.getDB("test").getMongo().startSession();
    const shardDB = session.getDatabase("test");
    const coll = shardDB.exchange_in_session;

    let bigString = '';
    for (let i = 0; i < 20; i++) {
        bigString += 's';
    }

    // Insert some documents.
    const nDocs = 50;
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.insert({_id: i, bigString: bigString}));
    }

    session.startTransaction();

    // Set up an Exchange with two cursors.
    let res = assert.commandWorked(shardDB.runCommand({
        aggregate: coll.getName(),
        pipeline: [],
        exchange: {
            policy: 'keyRange',
            consumers: NumberInt(2),
            key: {_id: 1},
            boundaries: [{a: MinKey}, {a: nDocs / 2}, {a: MaxKey}],
            consumerIds: [NumberInt(0), NumberInt(1)],
            bufferSize: NumberInt(128)
        },
        cursor: {batchSize: 0},
    }));

    function spawnShellToIterateCursor(cursorId) {
        let code = `const cursor = ${tojson(cursorId)};`;
        code += `const sessionId = ${tojson(session.getSessionId())};`;
        code += `const collName = "${coll.getName()}";`;
        function iterateCursorWithNoDocs() {
            const getMoreCmd = {
                getMore: cursor.id,
                collection: collName,
                batchSize: 4,
                lsid: sessionId,
                txnNumber: NumberLong(0),
                autocommit: false
            };

            let resp = null;
            while (!resp || resp.cursor.id != 0) {
                resp = assert.commandWorked(db.runCommand(getMoreCmd));
            }
        }
        code += `(${iterateCursorWithNoDocs.toString()})();`;
        return startParallelShell(code, st.rs0.getPrimary().port);
    }

    let parallelShells = [];
    for (let curs of res.cursors) {
        parallelShells.push(spawnShellToIterateCursor(curs.cursor));
    }

    assert.soon(function() {
        for (let waitFn of parallelShells) {
            waitFn();
        }
        return true;
    });

    session.abortTransaction();

    st.stop();
})();
