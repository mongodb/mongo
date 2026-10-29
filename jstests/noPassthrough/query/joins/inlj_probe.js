/**
 * Repro test case derived from TPC-H Q5, validating fix that ensures SBE & the join optimizer know about the same predicate in INLJ.
 *
 * @tags: [requires_sbe, requires_fcv_91]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("join optimization wrong INLJ probe slot", function () {
    let conn;
    let coll;
    let orders;
    let lineitem;
    let supplier;
    let q5Pipeline;

    const expected = [{_id: null, n: 4}];
    const joinHint = {
        perSubsetLevelMode: [
            {level: NumberInt(0), mode: "CHEAPEST", hint: {node: NumberInt(3)}},
            {
                level: NumberInt(1),
                mode: "CHEAPEST",
                hint: {node: NumberInt(2), method: "INLJ", isLeftChild: false},
            },
            {
                level: NumberInt(2),
                mode: "CHEAPEST",
                hint: {node: NumberInt(1), method: "HJ", isLeftChild: true},
            },
            {
                level: NumberInt(3),
                mode: "CHEAPEST",
                hint: {node: NumberInt(0), method: "INLJ", isLeftChild: false},
            },
        ],
    };

    before(function () {
        conn = MongoRunner.runMongod({setParameter: {internalEnableJoinOptimization: true}});
        assert.neq(conn, null, "failed to start mongod");
        const db = conn.getDB(jsTestName());

        coll = db.getCollection(jsTestName());
        orders = db.getCollection(jsTestName() + "_orders");
        lineitem = db.getCollection(jsTestName() + "_lineitem");
        supplier = db.getCollection(jsTestName() + "_supplier");

        q5Pipeline = [
            {
                $lookup: {
                    from: orders.getName(),
                    as: "orders",
                    localField: "c_custkey",
                    foreignField: "o_custkey",
                },
            },
            {$unwind: "$orders"},
            {
                $lookup: {
                    from: lineitem.getName(),
                    as: "lineitem",
                    localField: "orders.o_orderkey",
                    foreignField: "l_orderkey",
                },
            },
            {$unwind: "$lineitem"},
            {
                $lookup: {
                    from: supplier.getName(),
                    as: "supplier",
                    localField: "lineitem.l_suppkey",
                    foreignField: "s_suppkey",
                    let: {c_nationkey: "$c_nationkey"},
                    pipeline: [{$match: {$expr: {$eq: ["$s_nationkey", "$$c_nationkey"]}}}],
                },
            },
            {$unwind: "$supplier"},
            {$group: {_id: null, n: {$sum: 1}}},
        ];

        coll.drop();
        orders.drop();
        lineitem.drop();
        supplier.drop();

        assert.commandWorked(
            supplier.insertMany([
                {s_suppkey: 10, s_nationkey: 0},
                {s_suppkey: 11, s_nationkey: 1},
                {s_suppkey: 12, s_nationkey: 0},
                {s_suppkey: 13, s_nationkey: 1},
            ]),
        );
        assert.commandWorked(
            coll.insertMany([
                {c_custkey: 100, c_nationkey: 0},
                {c_custkey: 101, c_nationkey: 1},
                {c_custkey: 102, c_nationkey: 0},
                {c_custkey: 103, c_nationkey: 1},
            ]),
        );
        assert.commandWorked(
            orders.insertMany([
                {o_orderkey: 1000, o_custkey: 100},
                {o_orderkey: 1001, o_custkey: 101},
                {o_orderkey: 1002, o_custkey: 102},
                {o_orderkey: 1003, o_custkey: 103},
            ]),
        );
        assert.commandWorked(
            lineitem.insertMany([
                {l_orderkey: 1000, l_suppkey: 10},
                {l_orderkey: 1001, l_suppkey: 11},
                {l_orderkey: 1002, l_suppkey: 10},
                {l_orderkey: 1003, l_suppkey: 13},
            ]),
        );

        // Index every join-predicate path for arrayness (and so the query can use INLJ).
        assert.commandWorked(supplier.createIndex({s_suppkey: 1}));
        assert.commandWorked(supplier.createIndex({s_nationkey: 1}));
        assert.commandWorked(lineitem.createIndex({l_orderkey: 1}));
        assert.commandWorked(lineitem.createIndex({l_suppkey: 1}));
        assert.commandWorked(orders.createIndex({o_custkey: 1}));
        assert.commandWorked(orders.createIndex({o_orderkey: 1}));
        assert.commandWorked(coll.createIndex({c_custkey: 1}));
        assert.commandWorked(coll.createIndex({c_nationkey: 1}));
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
    });

    it("returns correct results with the hinted join order", function () {
        assertArrayEq({
            expected,
            actual: coll.aggregate([{$_internalJoinHint: joinHint}].concat(q5Pipeline)).toArray(),
        });
    });

    it("returns correct results without the join hint", function () {
        assertArrayEq({expected, actual: coll.aggregate(q5Pipeline).toArray()});
    });
});
