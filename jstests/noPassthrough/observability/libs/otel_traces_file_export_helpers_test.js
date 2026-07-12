import {describe, it} from "jstests/libs/mochalite.js";
import {getFlatSpansList} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

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
