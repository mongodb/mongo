// Tests that create database and drop database succeed when the config servers are v4.4 and shard
// servers are v4.2.

(function() {
"use strict";

const st = new ShardingTest({
    shards: [
        {binVersion: "last-stable"},
    ],
    mongos: 1,
    other: {mongosOptions: {binVersion: "last-stable"}}
});

// Create a database by inserting into a collection.
assert.commandWorked(st.s.getDB("test").getCollection("foo").insert({x: 1}));

// Drop the database.
assert.commandWorked(st.s.getDB("test").dropDatabase());

st.stop();
})();
