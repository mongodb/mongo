/**
 * Test that $queryStats properly tokenizes aggregation commands, on mongod and mongos.
 * @tags: [requires_fcv_72]
 */
import {
    asFieldPath,
    asVarRef,
    getQueryStats,
    getQueryStatsAggCmd,
    kShellApplicationName
} from "jstests/libs/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kHashedDbName = "iDlS7h5jf5HHxWPJpeHRbA+jLTNNZaqxVVkplrEkfko=";
const kHashedCollName = "w6Ax20mVkbJu4wQWAMjL8Sl+DfXAr2Zqdc3kJRB7Oo0=";
const kHashedFieldA = "GDiF6ZEXkeo4kbKyKEAAViZ+2RHIVxBQV9S6b6Lu7gU=";
const kHashedFieldB = "m1xtUkfSpZNxXjNZYKwo86vGD37Zxmd2gtt+TXDO558=";

function verifyConsistentFields(key) {
    assert.eq({"db": `${kHashedDbName}`, "coll": `${kHashedCollName}`}, key.queryShape.cmdNs);
    assert.eq("aggregate", key.queryShape.command);
    assert.eq(kShellApplicationName, key.client.application.name);
}

function runTest(conn) {
    const db = conn.getDB("testDB");
    const admin = conn.getDB("admin");

    db.test.drop();
    db.otherColl.drop();
    assert.commandWorked(db.test.insert({a: "foobar", b: 15}));
    assert.commandWorked(db.test.insert({a: "foobar", b: 20}));
    assert.commandWorked(db.otherColl.insert({a: "foobar", price: 2.50}));

    // First checks proper tokenization on a basic pipeline.
    {
        db.test
            .aggregate([
                {$sort: {a: -1}},
                {$match: {a: {$regex: "foo(.*)"}, b: {$gt: 10}}},
                {$skip: 5},
            ])
            .toArray();

        const stats = getQueryStatsAggCmd(admin, {transformIdentifiers: true});

        assert.eq(1,
                  stats.length,
                  {allStats: getQueryStats(admin), metrics: db.serverStatus().metrics.queryStats});
        const key = stats[0].key;
        verifyConsistentFields(key);
        // Make sure there is no otherNss field when there are no secondary namespaces.
        assert(!key.hasOwnProperty('otherNss'), key);
        // Ensure the query stats key pipeline holds the raw input without optimization (e.g., the
        // $sort stays before the $match, as in the raw query).
        assert.eq(
            [
                {"$sort": {[kHashedFieldA]: -1}},
                {
                    "$match": {
                        "$and": [
                            {[kHashedFieldA]: {"$regex": "?string"}},
                            {[kHashedFieldB]: {"$gt": "?number"}}
                        ]
                    }
                },
                {"$skip": "?number"}
            ],
            key.queryShape.pipeline,
            key.queryShape.pipeline);
    }

    // Checks proper tokenization on another basic pipeline that is a subset of the original
    // pipeline to make sure there are separate query stats entries per separate query shape.
    {
        db.test.aggregate([{$match: {a: {$regex: "foo(.*)"}, b: {$gt: 0}}}]).toArray();
        const stats = getQueryStatsAggCmd(admin, {transformIdentifiers: true});

        assert.eq(2, stats.length);
        const key = stats[0].key;
        verifyConsistentFields(key);
        // Make sure there is no otherNss field when there are no secondary namespaces.
        assert(!key.hasOwnProperty('otherNss'), key);
        assert.eq([{
                      "$match": {
                          "$and": [
                              {[kHashedFieldA]: {"$regex": "?string"}},
                              {[kHashedFieldB]: {"$gt": "?number"}}
                          ]
                      }
                  }],
                  key.queryShape.pipeline,
                  key.queryShape.pipeline);
    }
    // Checks proper tokenization on a pipeline that involves a let variable and a $lookup stage
    // that has its own subpipeline and references another namespace.
    {
        const kHashedOtherCollName = "8Rfz9QKu4P3BbyJ3Zpf5kxlUGx7gMvVk2PXZlJVfikE=";
        const kHashedAsOutputName = "OsoJyz+7myXF2CkbE5dKd9DJ1gDAUw5uyt12k1ENQpY=";
        const kHashedFieldOrderName = "KcpgS5iaiD5/3BKdQRG5rodz+aEE9FkcTPTYZ+G7cpA=";
        const kHashedFieldPrice = "LiAftyHzrbrVhwtTPaiHd8Lu9gUILkWgcP682amX7lI=";
        const kHashedFieldMaxPrice = "lFzklZZ6KbbYMBTi8KtTTp1GZCcPaUKUmOe3iko+IF8=";
        const kHashedFieldRole = "SGZr91N1v3SFufKI5ww9WSZ4krOXKRpxpS+QshHwyUk=";

        db.test.aggregate([{
            $lookup: {
                from: "otherColl",
                let: { order_name: "$a", price: "$price"},
                pipeline: [{
                    $match: {
                        $expr: {
                            $and: [
                                { $eq: ["$a", "$$order_name"] },
                                { $lte: ["$$price", "$$max_price"] }
                            ]
                        }
                    }
                }],
                as: "my_output"
            }},
        {
            $match: {$expr: {$eq: ["$role", "$$USER_ROLES.role"]}}
        }], {let: {max_price: 3.00}}).toArray();
        const stats = getQueryStatsAggCmd(admin, {transformIdentifiers: true});

        assert.eq(3, stats.length);
        // This one will sort last because of the 'let' parameters.
        const key = stats[2].key;
        verifyConsistentFields(key);
        assert.eq(
            [
                {
                    "$lookup": {
                        "from": `${kHashedOtherCollName}`,
                        "as": `${kHashedAsOutputName}`,
                        "let": {
                            [kHashedFieldOrderName]: asFieldPath(kHashedFieldA),
                            [kHashedFieldPrice]: asFieldPath(kHashedFieldPrice)
                        },
                        "pipeline": [{
                            "$match": {
                                "$expr": {
                                    "$and": [
                                        {
                                            "$eq": [
                                                asFieldPath(kHashedFieldA),
                                                asVarRef(kHashedFieldOrderName)
                                            ],
                                        },
                                        {
                                            "$lte": [
                                                asVarRef(kHashedFieldPrice),
                                                asVarRef(kHashedFieldMaxPrice)
                                            ]
                                        }
                                    ]
                                }
                            }
                        }]
                    }
                },
                {
                    "$match": {
                        "$expr": {
                            "$eq": [
                                asFieldPath(kHashedFieldRole),
                                asVarRef("USER_ROLES." + kHashedFieldRole)
                            ]
                        }
                    }
                }
            ],
            key.queryShape.pipeline,
            key.queryShape.pipeline);
        assert.eq({[kHashedFieldMaxPrice]: "?number"}, key.queryShape.let);
        assert.eq([{"db": `${kHashedDbName}`, "coll": `${kHashedOtherCollName}`}], key.otherNss);
    }
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsRateLimit: -1,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryStatsRateLimit: -1,
            'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
        }
    },
});
runTest(st.s);
st.stop();
