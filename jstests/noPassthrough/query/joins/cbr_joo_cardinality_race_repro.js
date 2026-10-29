/**
 * Regression test for SERVER-133645: the JOO/CBR collection-cardinality snapshot race.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

describe("JOO/CBR cardinality snapshot race", function () {
    let conn;
    let dbName;
    let orders;
    let cbrPlanning;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: false,
            },
        });
        assert.neq(conn, null, "failed to start mongod");

        dbName = jsTestName();
        const db = conn.getDB(dbName);
        orders = db.orders;
        const customers = db.customers;

        orders.drop();
        customers.drop();

        const docs = Array.from({length: 100}, (_, i) => ({_id: i, a: i}));
        assert.commandWorked(orders.insertMany(docs));
        assert.commandWorked(customers.insertMany(docs));
        assert.commandWorked(orders.createIndex({a: 1}));
        assert.commandWorked(customers.createIndex({a: 1}));
    });

    it("does not mix collection cardinality snapshots during CBR costing", function () {
        cbrPlanning = configureFailPoint(conn, "sleepWhileCbrPlanningForJoinOptimization");
        let awaitQuery;
        try {
            awaitQuery = startParallelShell(
                funWithArgs(function (queryDbName) {
                    const queryDB = db.getSiblingDB(queryDbName);
                    // The predicate matches all 100 sampled documents, so CBR estimates a
                    // cardinality of 100. The concurrent delete makes the current catalog
                    // cardinality 99, which reproduces the snapshot mismatch this test covers.
                    const result = queryDB.orders
                        .aggregate([
                            {$match: {a: {$gte: 0}}},
                            {
                                $lookup: {
                                    from: "customers",
                                    localField: "a",
                                    foreignField: "a",
                                    as: "customer",
                                },
                            },
                            {$unwind: "$customer"},
                        ])
                        .toArray();
                    assert.gt(result.length, 0, {result});
                }, dbName),
                conn.port,
            );

            assert(cbrPlanning.wait(), "JOO did not reach the CBR planning failpoint");
            assert.commandWorked(orders.deleteOne({_id: 0}));
        } finally {
            cbrPlanning.off();
            if (awaitQuery) {
                awaitQuery();
            }
        }
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
    });
});
