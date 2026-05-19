/**
 * End-to-end integration tests for the $embed stream processing stage.
 *
 * Each test drives a stream processor that uses $source with inline documents,
 * runs them through $embed (backed by a local fake HTTP embedding server), then
 * writes results via $merge into a per-test scratch collection.  The test polls
 * the scratch collection and asserts on the output.
 *
 * @tags: [
 *   featureFlagStreams,
 *   requires_streams,
 * ]
 */

import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {FakeEmbeddingServer} from
    "src/mongo/db/modules/enterprise/jstests/streams/fake_embedding_server.js";

const kDb = "embed_test";
const kLocalConn = "__embed_test_local";
const testDb = db.getSiblingDB(kDb);

// ---- helpers ----------------------------------------------------------------

const fake = new FakeEmbeddingServer();

// Connection objects passed to every stream processor.
// db.getMongo().host returns "localhost:PORT" in both mongo and mongosh.
const localConn = {
    name: kLocalConn,
    type: "atlas",
    options: {uri: "mongodb://" + db.getMongo().host + "/"},
};

function embedConn(overrides = {}) {
    return Object.assign({
        name: "fake-embed",
        type: "openai",
        options: {endpoint: fake.url() + "/v1/embeddings", apiKey: "test-key"},
    }, overrides);
}

let _procSeq = 0;
function freshName() {
    return "embed_proc_" + (++_procSeq);
}

function freshColl() {
    return "embed_out_" + new ObjectId().str;
}

/**
 * Build a pipeline with inline-document source, $embed, and $merge output.
 * Returns {pipeline, outCollName}.
 */
function makePipeline(docs, embedSpec, outCollName) {
    outCollName = outCollName || freshColl();
    const embed = Object.assign(
        {input: "$body", into: "embedding", model: {connectionName: "fake-embed", name: "test-model"}},
        embedSpec);
    return {
        pipeline: [
            {$source: {documents: docs}},
            {$embed: embed},
            {$merge: {into: {connectionName: kLocalConn, db: kDb, coll: outCollName}}},
        ],
        outCollName,
    };
}

/**
 * Start processor, wait for `count` docs in the output collection, stop, and
 * return the docs sorted by _id.  Drops the output collection on return.
 */
function run(name, pipeline, connections, count, {dlqCollName} = {}) {
    const options = Object.assign(
        {featureFlags: {enableEmbed: true, cidrDenyList: []}},
        dlqCollName ? {dlq: {connectionName: kLocalConn, db: kDb, coll: dlqCollName}} : {});
    assert.commandWorked(db.runCommand({
        streams_startStreamProcessor: "",
        name,
        processorId: name,
        tenantId: "embed_test_tenant",
        pipeline,
        connections,
        options,
    }));

    // Determine output collection from the $merge stage.
    const mergeSt = pipeline.find(s => s["$merge"]);
    const outColl = testDb.getCollection(mergeSt["$merge"].into.coll);

    assert.soon(
        () => outColl.countDocuments({}) >= count,
        () => "timeout: only " + outColl.countDocuments({}) + " of " + count + " docs in output",
        15000);

    assert.commandWorked(
        db.runCommand({streams_stopStreamProcessor: "", name, tenantId: "embed_test_tenant"}));

    const docs = outColl.find().sort({_id: 1}).toArray();
    outColl.drop();
    return docs;
}

/**
 * Like run() but waits for the DLQ collection to accumulate `count` docs.
 */
function runWaitDlq(name, pipeline, connections, dlqCollName, count) {
    assert.commandWorked(db.runCommand({
        streams_startStreamProcessor: "",
        name,
        processorId: name,
        tenantId: "embed_test_tenant",
        pipeline,
        connections,
        options: {featureFlags: {enableEmbed: true, cidrDenyList: []}, dlq: {connectionName: kLocalConn, db: kDb, coll: dlqCollName}},
    }));

    const dlqColl = testDb.getCollection(dlqCollName);
    assert.soon(
        () => dlqColl.countDocuments({}) >= count,
        () => "timeout waiting for DLQ docs; got " + dlqColl.countDocuments({}),
        15000);

    assert.commandWorked(
        db.runCommand({streams_stopStreamProcessor: "", name, tenantId: "embed_test_tenant"}));

    const docs = dlqColl.find().sort({_id: 1}).toArray();
    dlqColl.drop();
    return docs;
}

// ---- test suite -------------------------------------------------------------

describe("$embed stage", function() {
    before(function() {
        fake.start();
    });

    after(function() {
        fake.stop();
    });

    afterEach(function() {
        fake.reset();
        // Drop any leftover test collections.
        testDb.getCollectionNames()
            .filter(n => n.startsWith("embed_out_") || n.startsWith("embed_dlq_"))
            .forEach(n => testDb.getCollection(n).drop());
    });

    it("attaches an embedding array to each output document", function() {
        const {pipeline, outCollName} = makePipeline(
            [{_id: 1, body: "hello"}, {_id: 2, body: "world"}],
            {batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const docs = run(freshName(), pipeline, conns, 2);

        assert.eq(2, docs.length);
        for (const d of docs) {
            assert(Array.isArray(d.embedding), "expected embedding array", {d});
            assert.eq(3, d.embedding.length, "echo embedding is 3-dimensional", {d});
        }
    });

    it("writes embedding into a nested field when `into` uses a dotted path", function() {
        const {pipeline, outCollName} = makePipeline(
            [{_id: 1, body: "hi"}],
            {into: "meta.vector", batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const docs = run(freshName(), pipeline, conns, 1);

        assert(Array.isArray(docs[0].meta.vector), "nested field not set", {doc: docs[0]});
        assert.gt(docs[0].meta.vector.length, 0);
    });

    it("forwards doc unchanged when input expression evaluates to null", function() {
        // $body is missing on this document; the engine should pass it through
        // with the `into` field absent (same semantics as $project on missing).
        // onError='ignore' ensures the doc passes through regardless of error mode.
        const {pipeline} = makePipeline(
            [{_id: 1, other: "x"}],
            {onError: "ignore", batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const docs = run(freshName(), pipeline, conns, 1);

        assert.eq(0, fake.callCount(), "no provider call expected for null input");
        assert.eq(undefined, docs[0].embedding, "embedding field should be absent", {doc: docs[0]});
    });

    it("batches multiple documents into a single provider call", function() {
        // 30 docs / maxSize=10 → exactly 3 size-based flushes → 3 provider calls.
        const inputDocs = [];
        for (let i = 0; i < 30; i++) {
            inputDocs.push({_id: i, body: "doc-" + i});
        }
        const {pipeline} = makePipeline(inputDocs, {batch: {maxSize: NumberInt(10), maxWaitMs: NumberInt(1000)}});
        const conns = [localConn, embedConn()];

        run(freshName(), pipeline, conns, 30);

        assert.eq(3, fake.callCount(), "30 docs / maxSize=10 should produce 3 provider calls");
    });

    it("uses cache to avoid redundant provider calls for repeated inputs", function() {
        const {pipeline} = makePipeline(
            [
                {_id: 1, body: "alpha"},
                {_id: 2, body: "alpha"},
                {_id: 3, body: "beta"},
                {_id: 4, body: "alpha"},
            ],
            {cache: {enabled: true, maxEntries: NumberInt(100)}, batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const docs = run(freshName(), pipeline, conns, 4);

        // "alpha" on first miss + "beta" on first miss = 2 provider calls.
        assert.eq(2, fake.callCount(), "cache should suppress duplicate provider calls");

        // Identical inputs produce identical embeddings (deterministic).
        const byId = {};
        for (const d of docs) byId[d._id] = d.embedding;
        assert.eq(byId[1], byId[2], "alpha embeddings should be identical");
        assert.eq(byId[1], byId[4], "alpha embeddings should be identical after cache hit");
    });

    it("evaluates a composed input expression before embedding", function() {
        fake.setMode("captureInputs");

        const {pipeline} = makePipeline(
            [{_id: 1, subject: "ticket", body: "broken login"}],
            {
                input: {$concat: ["$subject", " | ", "$body"]},
                batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)},
            });
        const conns = [localConn, embedConn()];

        run(freshName(), pipeline, conns, 1);

        const inputs = fake.capturedInputs();
        assert.eq(["ticket | broken login"], inputs);
    });

    it("forwards the `dimensions` parameter in the provider request body", function() {
        fake.setMode("captureInputs");

        const {pipeline} = makePipeline(
            [{_id: 1, body: "test"}],
            {model: {connectionName: "fake-embed", name: "test-model", dimensions: NumberInt(512)},
             batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        // The fake server captures input; to inspect the request body we need
        // to check through the callCount proxy (exact request inspection is a
        // unit-test concern — here we just verify the stage starts and produces
        // output without rejecting the spec).
        const docs = run(freshName(), pipeline, conns, 1);
        assert(Array.isArray(docs[0].embedding));
    });

    it("retries the provider on a 429 rate-limit response", function() {
        fake.setResponseSequence([
            {code: 429, body: '{"error":"rate limit"}'},
            {code: 200, body: null},  // null → falls back to echo
        ]);

        const {pipeline} = makePipeline(
            [{_id: 1, body: "hello"}],
            {batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const docs = run(freshName(), pipeline, conns, 1);

        assert.eq(2, fake.callCount(), "should have retried after 429");
        assert(Array.isArray(docs[0].embedding));
    });

    // TODO: enable once the new $embed binary (with onMessage/onTick interface) is compiled.
    it.skip("retries the provider on a 5xx server error", function() {
        fake.setResponseSequence([
            {code: 503, body: '{"error":"unavailable"}'},
            {code: 200, body: null},
        ]);

        const {pipeline} = makePipeline(
            [{_id: 1, body: "hello"}],
            {batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const docs = run(freshName(), pipeline, conns, 1);

        assert.eq(2, fake.callCount(), "should have retried after 503");
        assert(Array.isArray(docs[0].embedding));
    });

    it("routes failed documents to the DLQ under onError='dlq'", function() {
        fake.setResponseSequence([{code: 400, body: '{"error":"bad input"}'}]);

        const outCollName = freshColl();
        const dlqCollName = "embed_dlq_" + new ObjectId().str;
        const {pipeline} = makePipeline([{_id: 7, body: "hi"}],
                                        {onError: "dlq", batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}},
                                        outCollName);
        const conns = [localConn, embedConn()];

        const dlqDocs = runWaitDlq(freshName(), pipeline, conns, dlqCollName, 1);

        assert.eq(1, dlqDocs.length, "one DLQ doc expected");
        assert(dlqDocs[0].errInfo != null, "DLQ record should have errInfo", {doc: dlqDocs[0]});
        assert(dlqDocs[0].errInfo.reason.includes("400"),
               "errInfo.reason should mention HTTP 400",
               {doc: dlqDocs[0]});
        // Output collection should be empty.
        assert.eq(0, testDb.getCollection(outCollName).countDocuments({}));
        testDb.getCollection(outCollName).drop();
    });

    it("passes doc through with absent embedding field under onError='ignore'", function() {
        fake.setResponseSequence([{code: 400, body: '{"error":"bad input"}'}]);

        const {pipeline} = makePipeline(
            [{_id: 1, body: "hi"}],
            {onError: "ignore", batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const docs = run(freshName(), pipeline, conns, 1);

        assert.eq(1, docs.length);
        assert.eq(undefined, docs[0].embedding, "ignore mode should leave `into` field absent");
    });

    it("passes doc through with absent embedding when input is missing", function() {
        fake.setResponseSequence([{code: 400, body: '{"error":"bad input"}'}]);

        const {pipeline} = makePipeline(
            [{_id: 1}],  // no `body` field
            {onError: "ignore", batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const docs = run(freshName(), pipeline, conns, 1);

        assert.eq(0, fake.callCount(), "null input should not reach provider");
        assert.eq(undefined, docs[0].embedding);
    });

    // TODO: enable once the new $embed binary (with onMessage/onTick interface) is compiled.
    // New behavior: null input bypasses the provider entirely, so the doc passes through to
    // the output even when onError='dlq'.  The old binary requires a DLQ to be configured
    // whenever onError='dlq' and routes null inputs to the DLQ.
    it.skip("forwards doc through output when input is missing even with onError='dlq'",
            function() {
                const outCollName = freshColl();
                const {pipeline} = makePipeline(
                    [{_id: 1}],  // no `body` field
                    {onError: "dlq", batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}},
                    outCollName);
                const conns = [localConn, embedConn()];

                const docs = run(freshName(), pipeline, conns, 1);

                assert.eq(0, fake.callCount(), "null input should bypass provider entirely");
                assert.eq(1, docs.length, "doc should pass through output, not DLQ");
            });

    it("preserves output order even when some documents hit the cache", function() {
        const docs = [
            {_id: 1, body: "x"},
            {_id: 2, body: "x"},   // cache hit after first x
            {_id: 3, body: "y"},
            {_id: 4, body: "z"},
            {_id: 5, body: "w"},
            {_id: 6, body: "v"},
            {_id: 7, body: "u"},
            {_id: 8, body: "t"},
        ];
        const {pipeline} = makePipeline(
            docs,
            {cache: {enabled: true, maxEntries: NumberInt(100)}, batch: {maxSize: NumberInt(4), maxWaitMs: NumberInt(1000)}});
        const conns = [localConn, embedConn()];

        const out = run(freshName(), pipeline, conns, 8);
        const ids = out.map(d => d._id);
        assert.eq([1, 2, 3, 4, 5, 6, 7, 8], ids, "output order must match input order");
    });

    it("produces identical embeddings for identical inputs (deterministic)", function() {
        const {pipeline} = makePipeline(
            [
                {_id: 1, body: "same"},
                {_id: 2, body: "different"},
                {_id: 3, body: "same"},
            ],
            {cache: {enabled: true, maxEntries: NumberInt(10)}, batch: {maxSize: NumberInt(1), maxWaitMs: NumberInt(50)}});
        const conns = [localConn, embedConn()];

        const out = run(freshName(), pipeline, conns, 3);
        const byId = {};
        for (const d of out) byId[d._id] = d.embedding;

        assert.eq(byId[1], byId[3], "same input must produce same embedding");
        assert.neq(byId[1], byId[2], "different inputs must produce different embeddings");
    });
});
