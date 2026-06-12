/**
 * SERVER-125720: $match/$expr/$or over dotted paths with $sort and $group when
 * internalQueryPlannerPushdownFilterToIxscanForSort is enabled.
 *
 * @tags: [
 *   requires_fcv_90,
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_index_creation,
 *   does_not_support_stepdowns,
 * ]
 */

import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("$expr/$or with dotted paths, $sort, and $group with filter-to-ixscan optimization", function () {
    before(function () {
        this.coll = assertDropAndRecreateCollection(db, "index_for_sort_expr_group");
        assert.commandWorked(this.coll.createIndex({"a.b": 1, c: 1}));
        assert.commandWorked(this.coll.insertMany([{}, {c: 0}, {a: {b: 0}}, {c: 1}]));
    });

    after(function () {
        assertDropCollection(db, this.coll.getName());
    });

    it("produces the same results as a collection scan", function () {
        const pipeline = [
            {$match: {$expr: {$or: [{$eq: ["$c", 0]}, {$eq: ["$a.b", 0]}]}}},
            {$sort: {"a.b": 1}},
            {$group: {_id: null, first: {$first: "$a.b"}}},
        ];
        runWithParamsAllNonConfigNodes(
            db,
            {internalQueryPlannerPushdownFilterToIxscanForSort: true},
            () => {
                const ixResults = this.coll.aggregate(pipeline).toArray();
                const csResults = this.coll.aggregate(pipeline, {hint: {$natural: 1}}).toArray();
                assert.sameMembers(ixResults, csResults);
            },
        );
    });
});
