/**
 * Verifies that the per-scope JS heap limit (jsHeapLimitMB) does not produce false OOM errors
 * under concurrency, scope pool reuse, and mixed allocation patterns. Both
 * jsUseLegacyMemoryTracking modes must produce zero OOM errors.
 *
 * @tags: [
 *      requires_scripting,
 *      mozjs_wasm_unsupported,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";

const JS_HEAP_LIMIT_MB = 50;
const conn = MongoRunner.runMongod({setParameter: {jsHeapLimitMB: JS_HEAP_LIMIT_MB}});
const testDB = conn.getDB("js_mozjs_heap_limit");
const coll = testDB.coll;

assert.commandWorked(coll.insertMany(Array.from({length: 100}, (_, i) => ({_id: i, n: 0}))));

// ---------------------------------------------------------------------------
// Concurrent $where
//
// Each $where evaluation checks out a MozJSImplScope from the scope pool,
// which owns a full SpiderMonkey JSContext. SpiderMonkey backs its GC arena
// with mmap'd 1 MB chunks (MapAlignedPages -> record_mmap_alloc); a scope
// evaluating a trivial predicate like this one uses ~5 MB of mmap.
//
// Overcounting risk: if the mmap counter is not scoped per-thread, concurrent
// scopes inflate each other's apparent usage. With 16 threads each holding
// ~5 MB of GC arena, a shared counter would show ~80 MB per scope -- well
// over the 50 MB jsHeapLimitMB -- causing ExceededMemoryLimit on scope init
// even though each individual scope is using only ~5 MB (10% of the limit).
// ---------------------------------------------------------------------------

function concurrentWorker(host, dbName, iterations) {
    const db = new Mongo(host).getDB(dbName);
    let oom = 0,
        other = 0,
        ok = 0;
    for (let i = 0; i < iterations; i++) {
        try {
            const res = db.runCommand({
                findAndModify: "coll",
                query: {_id: Math.floor(Math.random() * 100), $where: "this._id !== undefined"},
                update: {$inc: {n: 1}},
            });
            res.ok === 1
                ? ok++
                : res.code === ErrorCodes.ExceededMemoryLimit || res.code === ErrorCodes.JSInterpreterFailure
                  ? oom++
                  : other++;
        } catch (e) {
            e.code === ErrorCodes.ExceededMemoryLimit || e.code === ErrorCodes.JSInterpreterFailure ? oom++ : other++;
        }
    }
    return {oom, other, ok};
}

function runConcurrent(threads, iterations) {
    const ts = Array.from(
        {length: threads},
        () => new Thread(concurrentWorker, conn.host, testDB.getName(), iterations),
    );
    ts.forEach((t) => t.start());
    return ts.reduce(
        (acc, t) => {
            t.join();
            const r = t.returnData();
            return {oom: acc.oom + r.oom, other: acc.other + r.other, ok: acc.ok + r.ok};
        },
        {oom: 0, other: 0, ok: 0},
    );
}

for (const legacy of [false, true]) {
    jsTest.log.info(`Phase 1: concurrent $where, jsUseLegacyMemoryTracking=${legacy}`);
    assert.commandWorked(testDB.adminCommand({setParameter: 1, jsUseLegacyMemoryTracking: legacy}));
    const r = runConcurrent(16, 5);
    assert.eq(r.oom, 0, `jsUseLegacyMemoryTracking=${legacy}: unexpected OOM errors: ${tojson(r)}`);
    assert.eq(r.other, 0, `jsUseLegacyMemoryTracking=${legacy}: unexpected errors: ${tojson(r)}`);
}

// ---------------------------------------------------------------------------
// Scope pool reuse: sequential queries must not accumulate mmap_bytes
//
// $function checks out a scope from MozJSProxyScope's pool and returns it
// after each aggregation completes. new ArrayBuffer(1 MB) allocates its
// backing store through SpiderMonkey's BufferAllocator, which calls mmap
// directly (not the GC arena) and is tracked via record_mmap_alloc. When
// the scope is returned to the pool a GC runs and collects the ArrayBuffer;
// its finalizer calls UnmapPages -> record_mmap_free, driving mmap_bytes
// back to zero before the next checkout.
//
// Overcounting risk: if record_mmap_free is not called when the buffer is
// collected, or if the counter is not correctly associated with the scope's
// thread, mmap_bytes accumulates across reuse cycles. Per-iteration peak is
// ~5 MB. With a 50 MB limit, unchecked accumulation would cross the limit
// after ~9 additional iterations. Running 15 iterations gives clear headroom
// above the failure point while keeping the test fast.
// ---------------------------------------------------------------------------

jsTest.log.info("Phase 2: scope pool reuse, sequential mmap_bytes accumulation check");
assert.commandWorked(testDB.adminCommand({setParameter: 1, jsUseLegacyMemoryTracking: false}));
for (let i = 0; i < 15; i++) {
    const res = coll
        .aggregate([
            {$limit: 1},
            {
                $project: {
                    r: {
                        $function: {
                            body: function () {
                                // ArrayBuffer backing store -> BufferAllocator -> mmap -> record_mmap_alloc.
                                // Freed via UnmapPages -> record_mmap_free when the GC collects this buffer on scope return.
                                let b = new ArrayBuffer(1024 * 1024);
                                return new Uint8Array(b)[0];
                            },
                            args: [],
                            lang: "js",
                        },
                    },
                },
            },
        ])
        .toArray();
    assert.eq(res.length, 1, `scope reuse iteration ${i} failed`);
}

// ---------------------------------------------------------------------------
// Mixed allocations: arena objects + BufferAllocator mmap in same scope
//
// Exercises two allocation paths within a single $function invocation:
//   - 100 plain JS objects {x: j}: GC arena objects, bump-allocated from
//     mmap'd arena chunks. Creating many short-lived objects
//     triggers GC, exercising the record_mmap_free path as chunks are swept.
//   - new Uint8Array(200 * 1024): the 200 KB typed-array backing store is a
//     medium BufferAllocator allocation, sub-allocated from a mmap'd
//     BufferChunk.
//
// Overcounting risk: if mmap_bytes leaks across scope boundaries under
// concurrent load, the apparent total inflates. Per-scope total is ~5 MB
// << 50 MB limit, so any OOM error indicates a counting error rather than
// genuine memory pressure.
// ---------------------------------------------------------------------------

function mixedWorker(host, dbName, iterations) {
    const db = new Mongo(host).getDB(dbName);
    let oom = 0,
        other = 0;
    for (let i = 0; i < iterations; i++) {
        try {
            db.coll
                .aggregate([
                    {$limit: 1},
                    {
                        $project: {
                            r: {
                                $function: {
                                    body: function () {
                                        // Plain JS objects: GC arena (malloc_bytes).
                                        let s = [];
                                        for (let j = 0; j < 100; j++) s.push({x: j});
                                        // Typed array backing store: BufferAllocator mmap (mmap_bytes).
                                        return s.length + new Uint8Array(200 * 1024)[0];
                                    },
                                    args: [],
                                    lang: "js",
                                },
                            },
                        },
                    },
                ])
                .toArray();
        } catch (e) {
            e.code === ErrorCodes.ExceededMemoryLimit || e.code === ErrorCodes.JSInterpreterFailure ? oom++ : other++;
        }
    }
    return {oom, other};
}

jsTest.log.info("Phase 3: concurrent GC pressure + BufferAllocator mmap");
assert.commandWorked(testDB.adminCommand({setParameter: 1, jsUseLegacyMemoryTracking: false}));
const mixedTs = Array.from({length: 8}, () => new Thread(mixedWorker, conn.host, testDB.getName(), 3));
mixedTs.forEach((t) => t.start());
const mixed = mixedTs.reduce(
    (acc, t) => {
        t.join();
        const r = t.returnData();
        return {oom: acc.oom + r.oom, other: acc.other + r.other};
    },
    {oom: 0, other: 0},
);
assert.eq(mixed.oom, 0, `mixed allocation test: unexpected OOM errors: ${tojson(mixed)}`);
assert.eq(mixed.other, 0, `mixed allocation test: unexpected errors: ${tojson(mixed)}`);

// ---------------------------------------------------------------------------
// malloc_bytes stress: long strings allocated via StringBufferArena
//
// SpiderMonkey stores long string character data in a separate heap buffer
// allocated through StringBufferArena -> js_arena_malloc -> wrap_alloc ->
// malloc_bytes. Strings longer than ~23 chars exceed the inline limit and
// always use this path. 10,000 Latin-1 strings of 1000 chars = ~10 MB of
// malloc_bytes per scope. Combined with the ~5 MB GC arena baseline in
// mmap_bytes, the per-scope total is ~15 MB, well under the 50 MB limit.
//
// Overcounting risk: if malloc_bytes is not scoped per-thread, concurrent
// scopes inflate each other's apparent usage. With 8 threads each holding
// ~10 MB of string data, a shared counter would show ~80 MB per scope --
// far exceeding the 50 MB jsHeapLimitMB.
// ---------------------------------------------------------------------------

function mallocWorker(host, dbName, iterations) {
    const db = new Mongo(host).getDB(dbName);
    let oom = 0,
        other = 0;
    for (let i = 0; i < iterations; i++) {
        try {
            db.coll
                .aggregate([
                    {$limit: 1},
                    {
                        $project: {
                            r: {
                                $function: {
                                    body: function () {
                                        // Long strings: char data -> StringBufferArena -> js_malloc -> malloc_bytes.
                                        // 10,000 x 1000-char Latin-1 strings = ~10 MB of malloc_bytes.
                                        let strs = [];
                                        for (let j = 0; j < 10000; j++) {
                                            strs.push("x".repeat(1000));
                                        }
                                        return strs.length;
                                    },
                                    args: [],
                                    lang: "js",
                                },
                            },
                        },
                    },
                ])
                .toArray();
        } catch (e) {
            e.code === ErrorCodes.ExceededMemoryLimit || e.code === ErrorCodes.JSInterpreterFailure ? oom++ : other++;
        }
    }
    return {oom, other};
}

jsTest.log.info("Phase 4: concurrent malloc_bytes stress via long string allocations");
assert.commandWorked(testDB.adminCommand({setParameter: 1, jsUseLegacyMemoryTracking: false}));
const mallocTs = Array.from({length: 8}, () => new Thread(mallocWorker, conn.host, testDB.getName(), 3));
mallocTs.forEach((t) => t.start());
const mallocResult = mallocTs.reduce(
    (acc, t) => {
        t.join();
        const r = t.returnData();
        return {oom: acc.oom + r.oom, other: acc.other + r.other};
    },
    {oom: 0, other: 0},
);
assert.eq(mallocResult.oom, 0, `malloc stress test: unexpected OOM errors: ${tojson(mallocResult)}`);
assert.eq(mallocResult.other, 0, `malloc stress test: unexpected errors: ${tojson(mallocResult)}`);

MongoRunner.stopMongod(conn);
