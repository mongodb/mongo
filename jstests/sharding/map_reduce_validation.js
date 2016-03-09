var st = new ShardingTest({shards: 1});

var testDB = st.s.getDB('test');

var mapFunc = function() {
    emit(this.x, 1);
};
var reduceFunc = function(key, values) {
    return values.length;
};

assert.commandFailed(testDB.runCommand(
    {mapReduce: 'user', map: mapFunc, reduce: reduceFunc, out: {inline: 1, sharded: true}}));

testDB.bar.insert({i: 1});
assert.commandFailed(testDB.runCommand({
    mapReduce: 'bar',
    map: function() {
        emit(this.i, this.i * 3);
    },
    reduce: function(key, values) {
        return Array.sum(values);
    },
    out: {replace: "foo", db: "admin"}
}));

assert.commandFailed(testDB.runCommand({
    mapReduce: 'bar',
    map: function() {
        emit(this.i, this.i * 3);
    },
    reduce: function(key, values) {
        return Array.sum(values);
    },
    out: {replace: "foo", db: "config"}
}));

assert.commandWorked(testDB.runCommand({
    mapReduce: 'bar',
    map: function() {
        emit(this.i, this.i * 3);
    },
    reduce: function(key, values) {
        return Array.sum(values);
    },
    out: {replace: "foo", db: "test"}
}));

st.stop();
