(function() {

    var st = new ShardingTest({shards: 1});

    var testDB = st.s.getDB('test');
    assert.writeOK(testDB.foo.insert({a: 1}));

    var res =
        testDB.adminCommand({copydb: 1, fromhost: st.s.host, fromdb: 'test', todb: 'test_copy'});
    assert.commandWorked(res);

    var copy = st.s.getDB('test_copy');
    assert.eq(1, copy.foo.count());
    assert.eq(1, copy.foo.findOne().a);

    // Test invalid todb database name.
    assert.commandFailed(testDB.adminCommand(
        {copydb: 1, fromhost: st.s.host, fromdb: 'test_copy', todb: 'test/copy'}));

    st.stop();

})();
