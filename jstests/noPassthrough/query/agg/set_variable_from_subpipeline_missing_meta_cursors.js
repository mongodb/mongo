/**
 * A merging pipeline that sets $$SEARCH_META via $setVariableFromSubPipeline requires a cursor
 * source for its sub-pipeline. This test sends a merging command without that source directly to
 * a shard, bypassing the router-side validation, and expects the shard to fail the operation via
 * its tripwire assertion. No mongot is required: the failure is entirely in the merge-pipeline
 * wiring.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("merging pipeline with an unsourced $setVariableFromSubPipeline", function () {
    let st;
    let shardDB;
    let remoteCursorId;
    const collName = "search_coll";
    const lookupCollName = "assets";

    before(function () {
        st = new ShardingTest({shards: 2, mongos: 1});
        const dbName = jsTestName();
        const testDB = st.s.getDB(dbName);

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}),
        );
        assert.commandWorked(
            testDB[collName].insertMany([
                {_id: 0, title: "cakes", assetid: 1},
                {_id: 1, title: "kale", assetid: 2},
            ]),
        );
        assert.commandWorked(testDB[lookupCollName].insert({assetid: 1, name: "asset one"}));
        st.shardColl(testDB[collName], {_id: 1}, {_id: 1}, {_id: 1});

        // The merging command is internal, so present ourselves as an internal client. Internal
        // connections must attach an explicit readConcern to every command.
        const shardConn = st.rs1.getPrimary();
        const internalConn = new Mongo(shardConn.host);
        // Authenticates as __system when the suite runs with auth enabled; no-op otherwise.
        jsTest.authenticate(internalConn);
        assert.commandWorked(
            internalConn.adminCommand({
                hello: 1,
                internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(30)},
            }),
        );
        shardDB = internalConn.getDB(dbName);

        // A real cursor for $mergeCursors' data remote (same session as the merging command).
        const findRes = assert.commandWorked(
            shardDB.runCommand({
                find: collName,
                batchSize: 0,
                readConcern: {level: "local"},
            }),
        );
        remoteCursorId = findRes.cursor.id;
    });

    it("fails the operation when the metadata cursor source is missing", function () {
        // The merging-half command shape an affected router produces when delegating the merge
        // to a shard: the $setVariableFromSubPipeline sub-pipeline is the bare planShardedSearch
        // metaPipeline, with no cursor source.
        const mergingCmd = {
            aggregate: collName,
            pipeline: [
                {
                    $mergeCursors: {
                        sort: {$searchScore: -1},
                        compareWholeSortKey: false,
                        remotes: [
                            {
                                shardId: st.shard1.shardName,
                                hostAndPort: st.rs1.getPrimary().host,
                                cursorResponse: {
                                    cursor: {
                                        id: remoteCursorId,
                                        ns: shardDB[collName].getFullName(),
                                        firstBatch: [],
                                    },
                                    ok: 1,
                                },
                            },
                        ],
                        nss: shardDB[collName].getFullName(),
                        allowPartialResults: false,
                    },
                },
                {
                    $setVariableFromSubPipeline: {
                        setVariable: "$$SEARCH_META",
                        pipeline: [
                            {$group: {_id: {type: "$type"}, value: {$sum: "$count"}}},
                            {$replaceRoot: {newRoot: {count: {lowerBound: {$first: ["$value"]}}}}},
                        ],
                    },
                },
                {
                    $lookup: {
                        from: lookupCollName,
                        as: "asset",
                        localField: "assetid",
                        foreignField: "assetid",
                        let: {},
                        pipeline: [{$project: {name: 1, _id: 0}}],
                    },
                },
                {$unwind: {path: "$asset", preserveNullAndEmptyArrays: true}},
                {$addFields: {assetname: "$asset.name"}},
                {$project: {title: true, assetname: true, _id: false}},
                {$sort: {title: -1}},
                {$limit: 25},
            ],
            allowDiskUse: true,
            cursor: {batchSize: 101},
            readConcern: {level: "local"},
            fromMongos: true,
            let: {},
            collation: {locale: "simple"},
            writeConcern: {w: "majority"},
        };

        const res = shardDB.runCommand(mergingCmd);
        jsTest.log.info("Merging command result", {res});
        assert.commandFailedWithCode(res, 6448002);
    });

    after(function () {
        // The tripwire assertion (tassert) triggered above makes the shard exit with a non-zero
        // code at shutdown in testing environments, so skip exit-code validation.
        st.stop({skipValidatingExitCode: true});
    });
});
