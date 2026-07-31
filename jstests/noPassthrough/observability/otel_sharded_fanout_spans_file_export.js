/**
 * Tests that a scatter-gather query through mongos in a sharded cluster produces a span for each
 * mongos->mongod hop of the fanout, exported via the OTel JSONL file exporter.
 *
 * For a single trace we verify that each targeted shard's span for the command is the child of its
 * own mongos client (egress) span, and that all of those egress spans hang off one common mongos
 * span for the command. Together these show one mongos->mongod span per shard.
 *
 * @tags: [requires_otel_build, requires_sharding]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    acceptAllExternalTraces,
    createTraceDirectory,
    enableFullSampling,
    kNoSamplingStrategy,
    getServiceInstanceId,
    readClusterSpans,
    samplingConfigForStartup,
    showsFullFanout,
} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

describe("OTel sharded fanout span file export", function () {
    const kNumShards = 2;
    const kCommandName = "find";

    before(function () {
        const kOtelParams = {
            openTelemetryTracingFileFlushCount: 1,
            openTelemetryTracingBatchExportIntervalMillis: 500,
            featureFlagOtelTraceSampling: true,
            openTelemetryTracingSampling: samplingConfigForStartup({
                defaultSampling: kNoSamplingStrategy,
            }),
        };

        this.st = new ShardingTest({
            shards: kNumShards,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: {
                setParameter: {
                    ...kOtelParams,
                    opentelemetryTraceDirectory: createTraceDirectory(jsTestName() + "_mongos"),
                },
            },
            rsOptions: {
                setParameter: {
                    ...kOtelParams,
                    opentelemetryTraceDirectory: createTraceDirectory(jsTestName() + "_shard"),
                },
            },
        });

        this.shardPrimaries = [this.st.rs0.getPrimary(), this.st.rs1.getPrimary()];
        this.nodes = [this.st.s, ...this.shardPrimaries];

        for (const shard of this.shardPrimaries) {
            acceptAllExternalTraces(shard);
        }

        this.mongosId = getServiceInstanceId(this.st.s);
        this.shardIds = this.shardPrimaries.map((shard) => getServiceInstanceId(shard));

        // Shard a collection with data on both shards so a filter-less find must fan out to all.
        const dbName = "test";
        const collName = jsTestName();
        const ns = dbName + "." + collName;
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        assert.commandWorked(this.st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(this.st.s.adminCommand({split: ns, middle: {_id: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: ns,
                find: {_id: 0},
                to: this.st.shard1.shardName,
            }),
        );

        this.coll = this.st.s.getDB(dbName).getCollection(collName);
        assert.commandWorked(this.coll.insert([{_id: -1}, {_id: 1}]));

        // Sample only kCommandName at the mongos entry point, so that no other span starts a trace
        // here. Make each shard accept the externally-sampled (remote-parent) traces propagated by
        // mongos.
        enableFullSampling(this.st.s, {spanNames: [kCommandName]});
    });

    after(function () {
        this.st.stop();
    });

    it("emits a span for each mongos->mongod hop of a scatter-gather query", function () {
        const showsCompleteFanout = (group) =>
            showsFullFanout(group, {routerId: this.mongosId, shardIds: this.shardIds});

        // Run the find command that will trigger the fanout.
        assert.commandWorked(
            this.st.s.getDB("test").runCommand({
                find: this.coll.getName(),
                filter: {},
            }),
        );

        // Group this command's spans by trace, then look for a trace showing the full fanout.
        let spansByTrace = {};
        assert.soon(
            () => {
                spansByTrace = {};
                for (const span of readClusterSpans(this.nodes)) {
                    if (span.name !== kCommandName) {
                        continue;
                    }
                    (spansByTrace[span.traceId] ??= []).push(span);
                }
                return Object.values(spansByTrace).some(showsCompleteFanout);
            },
            () =>
                "No trace linked a mongos egress span per shard to that shard's ingress span for " +
                "the scatter-gather query: " +
                tojson(spansByTrace),
            30000,
            500,
        );
    });
});
