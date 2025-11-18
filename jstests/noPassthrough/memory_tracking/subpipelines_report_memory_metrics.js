/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregation
 * stages in subpipelines. Specifically, we are testing subpipelines that occur in these stages:
 * - $lookup
 * - $unionWith
 * - $facet
 *
 * @tags: [
 *   requires_profiling,
 *   requires_getmore,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_83,
 * ]
 */

import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const serverParams = {
    setParameter: {
        // Needed to avoid spilling to disk, which changes memory metrics.
        allowDiskUseByDefault: false,
        // Needed so that chunked memory tracking reaches CurOp
        internalQueryMaxWriteToCurOpMemoryUsageBytes: 256,
    },
};

const conn = MongoRunner.runMongod(serverParams);
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("test");
const testName = jsTestName();

jsTest.log.info("Testing $lookup with subpipeline memory tracking");
{
    const ordersCollName = testName + "_orders";
    const orderLinesCollName = testName + "_orderlines";
    const ordersColl = db[ordersCollName];
    const orderLinesColl = db[orderLinesCollName];

    ordersColl.drop();
    orderLinesColl.drop();

    const orderDocs = [
        {
            orderId: 1001,
            customerId: "CUST001",
            orderDate: new Date("2024-01-15"),
            status: "completed",
        },
        {
            orderId: 1002,
            customerId: "CUST002",
            orderDate: new Date("2024-01-16"),
            status: "pending",
        },
        {
            orderId: 1003,
            customerId: "CUST001",
            orderDate: new Date("2024-01-17"),
            status: "completed",
        },
        {
            orderId: 1004,
            customerId: "CUST003",
            orderDate: new Date("2024-01-18"),
            status: "shipped",
        },
    ];

    // Insert multiple line items per order.
    const orderLineDocs = [
        {orderLineId: 1, orderId: 1001, itemName: "Laptop", quantity: 1, unitPrice: 999.99},
        {orderLineId: 2, orderId: 1001, itemName: "Mouse", quantity: 2, unitPrice: 25.0},
        {orderLineId: 3, orderId: 1001, itemName: "Keyboard", quantity: 1, unitPrice: 75.0},
        {orderLineId: 4, orderId: 1002, itemName: "Monitor", quantity: 2, unitPrice: 299.99},
        {orderLineId: 5, orderId: 1002, itemName: "Webcam", quantity: 1, unitPrice: 89.99},
        {orderLineId: 6, orderId: 1003, itemName: "Tablet", quantity: 1, unitPrice: 499.99},
        {orderLineId: 7, orderId: 1003, itemName: "Stylus", quantity: 2, unitPrice: 49.99},
        {orderLineId: 8, orderId: 1003, itemName: "Case", quantity: 1, unitPrice: 29.99},
        {orderLineId: 9, orderId: 1004, itemName: "Phone", quantity: 1, unitPrice: 699.99},
        {orderLineId: 10, orderId: 1004, itemName: "Charger", quantity: 1, unitPrice: 39.99},
    ];

    assert.commandWorked(ordersColl.insertMany(orderDocs));
    assert.commandWorked(orderLinesColl.insertMany(orderLineDocs));

    const lookupPipeline = [
        {
            $lookup: {
                from: orderLinesCollName,
                localField: "orderId",
                foreignField: "orderId",
                as: "orderDetails",
                pipeline: [
                    {
                        $group: {
                            _id: "$orderId",
                            itemNames: {$push: "$itemName"}, // Concatenate all item names into a list
                            itemDetails: {
                                $push: {
                                    itemName: "$itemName",
                                    quantity: "$quantity",
                                    unitPrice: "$unitPrice",
                                    lineTotal: {$multiply: ["$quantity", "$unitPrice"]},
                                },
                            },
                            totalAmount: {$sum: {$multiply: ["$quantity", "$unitPrice"]}},
                            totalItems: {$sum: "$quantity"},
                        },
                    },
                ],
            },
        },
        {$unwind: "$orderDetails"},
        {$sort: {orderId: 1}},
    ];

    runMemoryStatsTest({
        db: db,
        collName: ordersCollName,
        commandObj: {
            aggregate: ordersCollName,
            pipeline: lookupPipeline,
            comment: "memory stats $lookup subpipeline test",
            allowDiskUse: false,
            cursor: {batchSize: 1},
        },
        stageName: "$group",
        expectedNumGetMores: 3, // 4 orders minus 1 for initial batch.
        skipInUseTrackedMemBytesCheck: true,
        // TODO SERVER-96383 $lookup does not provide very good explain output for subpipelines.
        skipExplainStageCheck: true,
        // Provide a path to the stage that contains a subpipeline. The test harness will look here
        // for the memory metrics.
        explainStageSubpipelinePath: {$lookup: {pipeline: {}}},
    });

    ordersColl.drop();
    orderLinesColl.drop();
}

jsTest.log.info("Testing $unionWith with subpipeline memory tracking");
{
    const mainCollName = testName + "_main";
    const unionCollName = testName + "_union";
    const mainColl = db[mainCollName];
    const unionColl = db[unionCollName];

    mainColl.drop();
    unionColl.drop();

    const mainDocs = [
        {_id: 1, category: "A", value: 10},
        {_id: 2, category: "B", value: 20},
        {_id: 3, category: "C", value: 15},
    ];

    const unionDocs = [
        {_id: 4, category: "D", value: 25},
        {_id: 5, category: "E", value: 30},
        {_id: 6, category: "F", value: 35},
        {_id: 7, category: "D", value: 25},
        {_id: 8, category: "E", value: 30},
        {_id: 9, category: "F", value: 35},
    ];

    assert.commandWorked(mainColl.insertMany(mainDocs));
    assert.commandWorked(unionColl.insertMany(unionDocs));

    const unionWithPipeline = [
        {$project: {category: "$category", "value": ["$value"]}},
        {
            $unionWith: {
                coll: unionCollName,
                pipeline: [{$group: {_id: "$category", value: {$push: "$value"}}}],
            },
        },
    ];

    runMemoryStatsTest({
        db: db,
        collName: mainCollName,
        commandObj: {
            aggregate: mainCollName,
            pipeline: unionWithPipeline,
            comment: "memory stats $unionWith subpipeline test",
            allowDiskUse: false,
            cursor: {batchSize: 4},
        },
        stageName: "group",
        expectedNumGetMores: 1,
        skipInUseTrackedMemBytesCheck: true,
        // Provide a path to the stage that contains a subpipeline. The test harness will look here
        // for the memory metrics.
        explainStageSubpipelinePath: {$unionWith: {pipeline: {}}},
    });

    mainColl.drop();
    unionColl.drop();
}

jsTest.log.info("Testing $facet with subpipelines memory tracking");
{
    // Set up collection for $facet test
    const facetCollName = testName + "_facet";
    const facetColl = db[facetCollName];

    facetColl.drop();

    // Insert test data
    const facetDocs = [
        {_id: 1, category: "A", value: 10},
        {_id: 2, category: "B", value: 20},
        {_id: 3, category: "A", value: 15},
        {_id: 4, category: "C", value: 25},
        {_id: 5, category: "A", value: 30},
        {_id: 6, category: "B", value: 35},
    ];

    assert.commandWorked(facetColl.insertMany(facetDocs));

    const facetPipeline = [
        {
            $facet: {
                byCategory: [
                    {
                        $group: {_id: "$category", values: {$push: "$value"}, total: {$sum: " $value"}},
                    },
                ],
                highValues: [{$match: {value: {$gte: 15}}}, {$sort: {value: -1}}, {$limit: 3}],
                scaled: [
                    {
                        $project: {
                            _id: null,
                            category: 1,
                            value: {$multiply: ["$value", 10]},
                        },
                    },
                ],
            },
        },
    ];

    runMemoryStatsTest({
        db: db,
        collName: facetCollName,
        commandObj: {
            aggregate: facetCollName,
            pipeline: facetPipeline,
            comment: "memory stats $facet subpipelines test",
            allowDiskUse: false,
            cursor: {batchSize: 1},
        },
        stageName: "group",
        expectedNumGetMores: 0, // $facet typically returns one document with all facets.
        skipInUseTrackedMemBytesCheck: true,
        // Provide a path to the stage that contains a subpipeline. The test harness will look here
        // for the memory metrics.
        explainStageSubpipelinePath: {$facet: {byCategory: {}}},
    });

    facetColl.drop();
}

MongoRunner.stopMongod(conn);
