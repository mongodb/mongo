import {workerThread} from "jstests/concurrency/fsm_libs/worker_thread.js";

async function shouldForwardErrorsFromAsyncRunCallback(conn) {
    const args = {
        host: conn.host,
        dbName: 'test',
        tid: 'thread0',
        clusterOptions: {sharded: false, replication: false},
        latch: new CountDownLatch(1),
        errorLatch: new CountDownLatch(1)
    };

    const res = await workerThread.main([], args, async function() {
        throw new Error('Thrown intentionally');
    });
    assert.eq(res.err,
              'Error: Thrown intentionally',
              'should forward errors thrown in async run callback');
}

const conn = MongoRunner.runMongod();
await shouldForwardErrorsFromAsyncRunCallback(conn);
MongoRunner.stopMongod(conn);
