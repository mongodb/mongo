
import {workerThread} from "jstests/concurrency/fsm_libs/worker_thread.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const tooManyArgsError =
    "Test content should not take parameters. If you intended to write callback-based content, use async functions instead.";

describe("no done", function() {
    it('should fail if done is passed to it', function() {
        let e = assert.throws(() => {
            it('fail', done => {});
        });
        assert.eq(e.message, tooManyArgsError);
    });
    [before, beforeEach, after, afterEach].forEach(hook => {
        it(`should fail if done is passed to ${hook.name}`, function() {
            let e = assert.throws(() => {
                hook(done => {});
            });
            assert.eq(e.message, tooManyArgsError);
        });
    });
});

let conn;
let log = [];
function asyncAppend(e) {
    return new Promise(resolve => {
        setTimeout(() => {
            log.push(e);
            resolve();
        }, 0);
    });
}

describe("async", function() {
    before(() => {
        assert.eq(log, []);
        conn = MongoRunner.runMongod();  // to use threads
    });

    describe("await", function() {
        before(async function() {
            log = [];
            await asyncAppend('before');
            assert.eq(log, ['before']);
        });
        beforeEach(async function() {
            assert.eq(log, ['before']);
            await asyncAppend('beforeEach');
            assert.eq(log, ['before', 'beforeEach']);
        });
        it('async - await', async function() {
            assert.eq(log, ['before', 'beforeEach']);
            await asyncAppend('test');
            assert.eq(log, ['before', 'beforeEach', 'test']);
        });
        afterEach(async function() {
            assert.eq(log, ['before', 'beforeEach', 'test']);
            await asyncAppend('afterEach');
            assert.eq(log, ['before', 'beforeEach', 'test', 'afterEach']);
        });
        after(async function() {
            assert.eq(log, ['before', 'beforeEach', 'test', 'afterEach']);
            await asyncAppend('after');
            assert.eq(log, ['before', 'beforeEach', 'test', 'afterEach', 'after']);
        });
    });

    // The async approach above is recommended and most natural, but is just syntactic sugar for the
    // following promise-based approach, which is also supported.
    describe("promise", function() {
        before(function() {
            log = [];
            return asyncAppend('before').then(() => {
                assert.eq(log, ['before']);
            });
        });
        beforeEach(function() {
            assert.eq(log, ['before']);
            return asyncAppend('beforeEach').then(() => {
                assert.eq(log, ['before', 'beforeEach']);
            });
        });
        it('returns promise', function() {
            assert.eq(log, ['before', 'beforeEach']);
            return asyncAppend('test').then(() => {
                assert.eq(log, ['before', 'beforeEach', 'test']);
            });
        });
        afterEach(function() {
            assert.eq(log, ['before', 'beforeEach', 'test']);
            return asyncAppend('afterEach').then(() => {
                assert.eq(log, ['before', 'beforeEach', 'test', 'afterEach']);
            });
        });
        after(function() {
            assert.eq(log, ['before', 'beforeEach', 'test', 'afterEach']);
            return asyncAppend('after').then(() => {
                assert.eq(log, ['before', 'beforeEach', 'test', 'afterEach', 'after']);
            });
        });
    });

    after(function() {
        assert.eq(log, ['before', 'beforeEach', 'test', 'afterEach', 'after']);
        MongoRunner.stopMongod(conn);
    });
});

// shim to look and feel like native setTimeout
function setTimeout(fn, ms) {
    const args = {
        host: conn.host,
        dbName: 'test',
        tid: 'thread0',
        clusterOptions: {sharded: false, replication: false},
        latch: new CountDownLatch(1),
        errorLatch: new CountDownLatch(1)
    };
    workerThread.main([], args, function() {
        sleep(ms);
        fn();
    });
}
