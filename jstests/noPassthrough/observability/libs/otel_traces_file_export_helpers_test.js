import {describe, it} from "jstests/libs/mochalite.js";
import {
    getFlatSpansList,
    showsFullFanout,
} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

describe("getFlatSpansList", function () {
    it("flattens spans across all resource and scope spans", function () {
        const record = {
            resourceSpans: [
                {scopeSpans: [{spans: [{name: "a"}, {name: "b"}]}, {spans: [{name: "c"}]}]},
                {scopeSpans: [{spans: [{name: "d"}]}]},
            ],
        };
        assert.eq(
            getFlatSpansList(record).map((s) => s.name),
            ["a", "b", "c", "d"],
        );
    });

    it("returns an empty array for missing or empty structures", function () {
        assert.eq(getFlatSpansList(undefined), []);
        assert.eq(getFlatSpansList({}), []);
        assert.eq(getFlatSpansList({resourceSpans: [{scopeSpans: [{spans: []}]}]}), []);
        assert.eq(getFlatSpansList({resourceSpans: [{}]}), []);
        assert.eq(getFlatSpansList({resourceSpans: [{scopeSpans: [{}]}]}), []);
    });
});

describe("showsFullFanout", function () {
    const kRouterId = "router";
    const kShardIds = ["shard0", "shard1"];

    // Builds a span as annotated by readClusterSpans(): the emitting node's resource attributes plus
    // the span's own id and its parent's.
    function span(instanceId, spanId, parentSpanId) {
        return {
            resource: {"service.instance.id": instanceId},
            spanId,
            parentSpanId,
        };
    }

    // A complete two-shard fanout: one router command span, one router egress span per shard, and
    // each shard's ingress span parented by its own egress span.
    function fullFanoutSpans() {
        return [
            span(kRouterId, "router-cmd", "0000000000000000"),
            span(kRouterId, "egress0", "router-cmd"),
            span(kRouterId, "egress1", "router-cmd"),
            span(kShardIds[0], "ingress0", "egress0"),
            span(kShardIds[1], "ingress1", "egress1"),
        ];
    }

    function check(spans) {
        return showsFullFanout(spans, {routerId: kRouterId, shardIds: kShardIds});
    }

    it("accepts a complete fanout", function () {
        assert(check(fullFanoutSpans()));
    });

    it("accepts a complete fanout regardless of span order", function () {
        assert(check(fullFanoutSpans().reverse()));
    });

    it("ignores unrelated spans in the trace", function () {
        const spans = fullFanoutSpans();
        spans.push(span(kRouterId, "other-router-span", "router-cmd"));
        spans.push(span("shard2", "other-shard-span", "egress0"));
        assert(check(spans));
    });

    it("rejects an empty span list", function () {
        assert(!check([]));
    });

    it("rejects a fanout that misses a shard", function () {
        const spans = fullFanoutSpans().filter((s) => s.spanId !== "ingress1");
        assert(!check(spans));
    });

    it("rejects a shard ingress span that is not parented by a router span", function () {
        const spans = fullFanoutSpans();
        // shard1's ingress span hangs off shard0's ingress span instead of a router egress span.
        spans.find((s) => s.spanId === "ingress1").parentSpanId = "ingress0";
        assert(!check(spans));
    });

    it("rejects a shard ingress span whose parent span is missing from the trace", function () {
        const spans = fullFanoutSpans().filter((s) => s.spanId !== "egress1");
        assert(!check(spans));
    });

    it("rejects shards sharing a single egress span", function () {
        const spans = fullFanoutSpans().filter((s) => s.spanId !== "egress1");
        spans.find((s) => s.spanId === "ingress1").parentSpanId = "egress0";
        assert(!check(spans));
    });

    it("rejects egress spans that do not share a common parent", function () {
        const spans = fullFanoutSpans();
        spans.push(span(kRouterId, "other-router-cmd", "0000000000000000"));
        spans.find((s) => s.spanId === "egress1").parentSpanId = "other-router-cmd";
        assert(!check(spans));
    });

    it("rejects a common egress parent that is missing from the trace", function () {
        const spans = fullFanoutSpans().filter((s) => s.spanId !== "router-cmd");
        assert(!check(spans));
    });

    it("rejects a common egress parent emitted by a node other than the router", function () {
        const spans = fullFanoutSpans();
        spans.find((s) => s.spanId === "router-cmd").resource["service.instance.id"] = "shard0";
        assert(!check(spans));
    });
});
