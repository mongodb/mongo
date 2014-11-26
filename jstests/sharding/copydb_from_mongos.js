var st = new ShardingTest({ shards: 2 });

var testDB = st.s.getDB('test');
testDB.foo.insert({ a: 1 });
assert.eq(null, testDB.getLastError());

var res = testDB.adminCommand({ copydb: 1,
                                fromhost: st.s.host,
                                fromdb: 'test',
                                todb: 'test_copy' });
assert.commandWorked(res);

var copy = st.s.getDB('test_copy');
assert.eq(1, copy.foo.count());
assert.eq(1, copy.foo.findOne().a);

st.stop();
