/**
 * Tests that readPref applies on an explain for an aggregation command.
 */
(function() {
    "use strict";

    load("jstests/libs/profiler.js");  // For profilerHasSingleMatchingEntryOrThrow.

    const st = new ShardingTest({
        name: "agg_explain_readPref",
        shards: 2,
        other: {
            rs0: {
                nodes: [
                    {rsConfig: {priority: 1, tags: {"tag": "primary"}}},
                    {rsConfig: {priority: 0, tags: {"tag": "secondary"}}}
                ]
            },
            rs1: {
                nodes: [
                    {rsConfig: {priority: 1, tags: {"tag": "primary"}}},
                    {rsConfig: {priority: 0, tags: {"tag": "secondary"}}}
                ]
            },
            enableBalancer: false
        }
    });

    const mongos = st.s;
    const config = mongos.getDB("config");
    const mongosDB = mongos.getDB("agg_explain_readPref");
    assert.commandWorked(mongosDB.dropDatabase());

    const coll = mongosDB.getCollection("coll");

    assert.commandWorked(config.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), "agg_explain_readPref-rs0");
    const rs0Primary = st.rs0.getPrimary();
    const rs0Secondary = st.rs0.getSecondary();
    const rs1Primary = st.rs1.getPrimary();
    const rs1Secondary = st.rs1.getSecondary();

    for (let i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    //
    // Confirms that aggregations with explain run against mongos are executed against a tagged
    // secondary or primary, as per readPreference setting.
    //
    function confirmReadPreference(primary, secondary) {
        assert.commandWorked(secondary.setProfilingLevel(2));
        assert.commandWorked(primary.setProfilingLevel(2));

        // [<pref>, <tags>, <target>, <comment>]
        [['primary', [{}], primary, "primary"],
         ['primaryPreferred', [{tag: 'secondary'}], primary, "primaryPreferred"],
         ['secondary', [{}], secondary, "secondary"],
         ['secondary', [{tag: 'secondary'}], secondary, "secondaryTag"],
         ['secondaryPreferred', [{tag: 'secondary'}], secondary, "secondaryPreferred"],
         ['secondaryPreferred', [{tag: 'primary'}], primary, "secondaryPreferredTagPrimary"]]
            .forEach(function(args) {
                const pref = args[0], tagSets = args[1], target = args[2], name = args[3];

                //
                // Tests that explain within an aggregate command and an explicit $readPreference
                // targets the correct node in the replica set given by 'target'.
                //
                let comment = name + "_explain_within_query";
                assert.commandWorked(mongosDB.runCommand({
                    query: {
                        aggregate: "coll",
                        pipeline: [],
                        comment: comment,
                        cursor: {},
                        explain: true
                    },
                    $readPreference: {mode: pref, tags: tagSets}
                }));

                // Look for an operation without an exception, since the shard throws a stale config
                // exception if the shard or mongos has stale routing metadata, and the operation
                // gets retried.
                profilerHasSingleMatchingEntryOrThrow(target, {
                    "ns": coll.getFullName(),
                    "command.explain.aggregate": coll.getName(),
                    "command.explain.comment": comment,
                    "command.$readPreference.mode": pref == 'primary' ? null : pref,
                    "exception": {"$exists": false}
                });

                //
                // Tests that an aggregation command wrapped in an explain with explicit
                // $queryOptions targets the correct node in the replica set given by 'target'.
                //
                comment = name + "_explain_wrapped_agg";
                assert.commandWorked(mongosDB.runCommand({
                    $query: {
                        explain: {
                            aggregate: "coll",
                            pipeline: [],
                            comment: comment,
                            cursor: {},
                        }
                    },
                    $readPreference: {mode: pref, tags: tagSets}
                }));

                // Look for an operation without an exception, since the shard throws a stale config
                // exception if the shard or mongos has stale routing metadata, and the operation
                // gets retried.
                profilerHasSingleMatchingEntryOrThrow(target, {
                    "ns": coll.getFullName(),
                    "command.explain.aggregate": coll.getName(),
                    "command.explain.comment": comment,
                    "command.$readPreference.mode": pref == 'primary' ? null : pref,
                    "exception": {"$exists": false}
                });
            });

        assert.commandWorked(secondary.setProfilingLevel(0));
        assert.commandWorked(primary.setProfilingLevel(0));
    }

    //
    // Test aggregate explains run against an unsharded collection.
    //
    confirmReadPreference(rs0Primary.getDB(mongosDB.getName()),
                          rs0Secondary.getDB(mongosDB.getName()));

    //
    // Test aggregate explains run against a sharded collection.
    //
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(config.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
    assert.commandWorked(mongos.adminCommand({split: coll.getFullName(), middle: {a: 6}}));
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: coll.getFullName(), find: {a: 25}, to: "agg_explain_readPref-rs1"}));

    // Sharded tests are run against the non-primary shard for the "agg_explain_readPref" db.
    confirmReadPreference(rs1Primary.getDB(mongosDB.getName()),
                          rs1Secondary.getDB(mongosDB.getName()));

    st.stop();
})();
