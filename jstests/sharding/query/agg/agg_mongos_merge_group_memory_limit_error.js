/**
 * mongos never applies the 'allowDiskUseByDefault' server parameter: unless the command sets
 * 'allowDiskUse' explicitly, a blocking merge stage ($group, $setWindowFields) runs on the router,
 * which cannot spill. Tests that exceeding the memory limit there produces an error telling the user
 * to pass allowDiskUse:true explicitly, that doing so moves the merge to a shard where it succeeds,
 * and that the same error thrown on a shard keeps the original advice.
 *
 * @tags: [requires_fcv_91]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kRouterMessageTail =
    "but mongos cannot spill to disk and ignores allowDiskUseByDefault; pass allowDiskUse:true to " +
    "run the merge on a shard";
const kShardAdvice = "pass allowDiskUse:true to opt in";
const kDefaultMemoryBytes = 100 * 1024 * 1024;

const kStages = [
    {
        name: "$group",
        trackerName: "$group",
        knob: "internalDocumentSourceGroupMaxMemoryBytes",
        // Grouping on a unique key means each shard returns ~200 groups, and the merging $group
        // sees all 400 of them.
        pipeline: [{$group: {_id: "$a", count: {$sum: 1}}}],
    },
    {
        name: "$setWindowFields",
        trackerName: "SPILLABLE_DEQUE",
        knob: "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
        pipeline: [
            {
                $setWindowFields: {
                    sortBy: {a: 1},
                    output: {s: {$sum: "$a", window: {documents: ["unbounded", "current"]}}},
                },
            },
        ],
    },
];

describe("router merge memory limit error", function () {
    let st;
    let coll;
    let shardNodes;

    function setOnAll(nodes, params) {
        nodes.forEach((node) =>
            assert.commandWorked(node.adminCommand({setParameter: 1, ...params})),
        );
    }

    before(function () {
        st = new ShardingTest({shards: 2, mongos: 1});
        const db = st.s0.getDB(jsTestName());
        coll = db[jsTestName()];
        shardNodes = st.rs0.nodes.concat(st.rs1.nodes);

        assert.commandWorked(
            db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 0},
                to: st.shard1.shardName,
            }),
        );

        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = -200; i < 200; i++) {
            bulk.insert({_id: i, a: i});
        }
        assert.commandWorked(bulk.execute());
    });

    after(function () {
        st.stop();
    });

    for (const stage of kStages) {
        // Only lower the limit on the router so the shards' partial work is unaffected.
        function withRouterLimit(fn) {
            setOnAll([st.s0], {[stage.knob]: 1024});
            try {
                fn();
            } finally {
                setOnAll([st.s0], {[stage.knob]: kDefaultMemoryBytes});
            }
        }

        for (const options of [{allowDiskUse: false}, {}]) {
            it(`${stage.name} merging on the router explains how to enable spilling (${tojson(options)})`, function () {
                // The server default is true, but mongos ignores it, so the merge stays on the
                // router both when allowDiskUse is false and when it is not specified.
                const defaultParam = assert.commandWorked(
                    st.s0.adminCommand({getParameter: 1, allowDiskUseByDefault: 1}),
                );
                assert.eq(true, defaultParam.allowDiskUseByDefault);
                assert.eq("router", coll.explain().aggregate(stage.pipeline, options).mergeType);

                withRouterLimit(() => {
                    const err = assert.throwsWithCode(
                        () => coll.aggregate(stage.pipeline, options).toArray(),
                        ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
                    );
                    const expected = `Exceeded memory limit for ${stage.trackerName}, ${kRouterMessageTail}`;
                    assert(err.message.includes(expected), "missing router message", {err});
                    assert(!err.message.includes(kShardAdvice), "unexpected shard advice", {err});
                });
            });
        }

        it(`${stage.name} succeeds when allowDiskUse:true moves the merge to a shard`, function () {
            const options = {allowDiskUse: true};
            assert.neq("router", coll.explain().aggregate(stage.pipeline, options).mergeType);
            withRouterLimit(() => {
                assert.eq(400, coll.aggregate(stage.pipeline, options).toArray().length);
            });
        });
    }

    it("keeps the allowDiskUse advice when the error originates on a shard", function () {
        const stage = kStages[0];
        // Force the classic engine so the shard's $group honors the memory knob rather than SBE's.
        setOnAll(shardNodes, {
            [stage.knob]: 1024,
            internalQueryFrameworkControl: "forceClassicEngine",
        });
        try {
            const err = assert.throwsWithCode(
                () => coll.aggregate(stage.pipeline, {allowDiskUse: false}).toArray(),
                ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
            );
            assert(err.message.includes(kShardAdvice), "missing shard advice", {err});
            assert(!err.message.includes(kRouterMessageTail), "unexpected router message", {
                err,
            });
        } finally {
            setOnAll(shardNodes, {
                [stage.knob]: kDefaultMemoryBytes,
                internalQueryFrameworkControl: "trySbeRestricted",
            });
        }
    });
});
