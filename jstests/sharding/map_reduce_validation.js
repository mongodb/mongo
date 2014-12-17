var st = new ShardingTest({ shards: 1 });

var testDB = st.s.getDB('test');

var mapFunc = function() { emit(this.x, 1); };
var reduceFunc = function(key, values) { return values.length; };

assert.commandFailed(testDB.runCommand({ mapReduce: 'user',
                                         map: mapFunc,
                                         reduce: reduceFunc,
                                         out: { inline: 1, sharded: true }}));

st.stop();
