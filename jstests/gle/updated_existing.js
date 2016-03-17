/**
* SERVER-5872 : This test checks that the return message "updatedExisting" of
*               an upsert is not missing when autosplit takes place.
*/

var st = new ShardingTest({shards: 1, mongos: 1, verbose: 1, chunkSize: 1});

var testDB = st.getDB("test");
var coll = "foo";
testDB[coll].drop();

st.adminCommand({enablesharding: 'test'});
st.adminCommand({shardcollection: 'test.' + coll, key: {"shardkey2": 1, "shardkey1": 1}});

var bigString = "";
while (bigString.length < 1024 * 50)
    bigString += "asocsancdnsjfnsdnfsjdhfasdfasdfasdfnsadofnsadlkfnsaldknfsad";

for (var i = 0; i < 10000; ++i) {
    testDB[coll].update({"shardkey1": "test" + i, "shardkey2": "test" + i},
                        {$set: {"test_upsert": bigString}},
                        true,    // upsert
                        false);  // multi
    assert.eq(testDB.getLastErrorObj().updatedExisting, false);
}

st.stop();
