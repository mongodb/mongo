/**
 * Runs stress tests of exchange producers, where multiple threads run concurrent getNexts on the
 * consumer cursors.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   # The config fuzzer runs logical session cache refreshes, which was occasionally killing the
 *   # cursors before the test is over.
 *   does_not_support_config_fuzzer,
 *   # This will fail if using transactions because the FSM will attempt to call getMore on a cursor
 *   # that's been created outside of the transaction.
 *   does_not_support_transactions
 * ]
 */

// The buffer needs to be big enough that no one consumer buffer gets filled near the final
// iteration, or it may hang if the final getMore is blocked waiting on a different consumer to make
// space in its buffer.
export const $config = (function() {
    var data = {
        numDocs: 10000,
        numConsumers: 5,
        bufferSize: 100 * 1024,
        batchSize: 5,
    };

    // Runs a getMore on the cursor with id at cursorIndex. We have enough documents and few enough
    // iterations that the cursors should never be exhausted.
    function runGetMoreOnCursor(db, collName, cursorIndex, batchSize, cursorIds, sessionId) {
        // See comment at the end of setup() for why we need eval().
        const cursorId = eval(cursorIds[cursorIndex]);
        const res = db.runCommand(
            {getMore: cursorId, collection: collName, batchSize, lsid: {id: eval(sessionId)}});

        // If the getMore was successful, assert we have enough results returned; otherwise, it
        // should have because another worker thread has that cursor in use.
        if (res.ok) {
            // All batches before the final batch must have batchSize results. The final batch will
            // have 0 results.
            assert.eq(batchSize, res.cursor.nextBatch.length);
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.CursorInUse);
        }
    }

    // One state per consumer consumer, with equal probability so we exhaust each cursor in
    // approximately the same timeline. See runGetMoreOnCursor for details.
    var states = function() {
        function makeConsumerCallback(consumerId) {
            return function consumerCallback(db, collName) {
                return runGetMoreOnCursor(
                    db, collName, consumerId, this.batchSize, this.cursorIds, this.sessionId);
            };
        }

        return {
            // A no-op starting state so the worker threads don't all start on the same cursors.
            init: function init(db, collName) {},
            consumer0: makeConsumerCallback(0),
            consumer1: makeConsumerCallback(1),
            consumer2: makeConsumerCallback(2),
            consumer3: makeConsumerCallback(3),
            consumer4: makeConsumerCallback(4),
        };
    }();

    var allStatesEqual =
        {init: 0, consumer0: 0.2, consumer1: 0.2, consumer2: 0.2, consumer3: 0.2, consumer4: 0.2};
    var transitions = {
        init: allStatesEqual,
        consumer0: allStatesEqual,
        consumer1: allStatesEqual,
        consumer2: allStatesEqual,
        consumer3: allStatesEqual,
        consumer4: allStatesEqual,
    };

    function setup(db, collName, cluster) {
        // Start a session so we can pass the sessionId from when we retrieved the cursors to the
        // getMores where we want to iterate the cursors.
        const session = db.getMongo().startSession();

        // Load data.
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, a: i % 5, b: "foo"});
        }
        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);
        assert.eq(this.numDocs, db[collName].find().itcount());

        // Run an exchange to get a list of cursors.
        res = assert.commandWorked(session.getDatabase(db.getName()).runCommand({
            aggregate: collName,
            pipeline: [],
            exchange: {
                policy: "roundrobin",
                consumers: NumberInt(this.numConsumers),
                bufferSize: NumberInt(this.bufferSize)
            },
            cursor: {batchSize: 0},
        }));

        // Save the cursor ids to $config.data so each of the worker threads has access to the
        // cursors, as well as the sessionId.
        assert.eq(this.numConsumers, res.cursors.length);
        this.sessionId = tojson(session.getSessionId()["id"]);
        this.cursorIds = [];
        for (const cursor of res.cursors) {
            // We have to stringify the cursorId with tojson() since serializing data between
            // threads in the mongo shell doesn't work. When we use it later, we rehydrate it with
            // eval().
            this.cursorIds.push(tojson(cursor.cursor.id));
        }
    }

    function teardown(db, collName) {
        // Kill all the open cursors.
        let cursors = [];
        for (const cursorId of this.cursorIds) {
            cursors.push(eval(cursorId));
        }
        assert.commandWorked(db.runCommand({killCursors: collName, cursors}));
    }

    // threadCount must be equal to numConsumers. We need as many worker threads as consumers to
    // avoid a deadlock where all threads are waiting for one particular cursor to run a getMore.
    return {
        threadCount: data.numConsumers,
        iterations: 20,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data
    };
})();
