/**
 * Test that the keys on config server are rotated according to the KeysRotationIntervalSec value
 */

(function() {
    "use strict";
    const kRotationInterval = 30;
    let st = new ShardingTest({
        mongos: 1,
        shards: {rs0: {nodes: 2}},
        mongosWaitsForKeys: true,
        other: {configOptions: {setParameter: "KeysRotationIntervalSec=30"}}
    });

    let keys = st.s.getDB("admin").system.keys.find();
    // add a few seconds to the expire timestamp to account for rounding that may happen.
    let maxExpireTime = Timestamp(Date.now() / 1000 + kRotationInterval * 2 + 5, 0);

    assert(keys.count() >= 2);
    keys.toArray().forEach(function(key, i) {
        assert.hasFields(
            key,
            ["purpose", "key", "expiresAt"],
            "key document " + i + ": " + tojson(key) + ", did not have all of the expected fields");
        assert.lte(bsonWoCompare(key.expiresAt, maxExpireTime),
                   0,
                   "key document " + i + ": " + tojson(key) + "expiresAt value is greater than: " +
                       maxExpireTime);
    });
    st.stop();
})();
