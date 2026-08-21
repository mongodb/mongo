// Pins that the field-stats read path's lookup shape (a point _id find with no projection, as
// issued by loadFieldStats()) is served by the express path.
import {before, after, describe, it} from "jstests/libs/mochalite.js";
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

describe("field_stats point lookup", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPersistentStats: true,
                internalQueryEnablePersistentNDVStats: true,
            },
        });
        this.db = this.conn.getDB("test");
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("uses the express path", function () {
        const coll = this.db[jsTestName()];
        assert.commandWorked(coll.insert([{a: 1}, {a: 2}]));
        assert.commandWorked(this.db.runCommand({analyze: coll.getName(), mode: "ndv", key: "a"}));

        // Look the document up by the _id 'analyze' actually wrote, so that a change to the _id
        // scheme is reflected here rather than pinning a stale shape.
        const statsColl = this.db["system.stats.field_stats"];
        const docs = statsColl.find().toArray();
        assert.eq(docs.length, 1, "expected one field-stats doc", {docs});

        const explain = statsColl.find({_id: docs[0]._id}).limit(1).explain();
        const plan = getWinningPlanFromExplain(explain);
        assert.eq(plan.stage, "EXPRESS_IXSCAN", "expected an express point lookup", {plan});
    });
});
