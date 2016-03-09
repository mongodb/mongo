// Test that dropping the config database is completely disabled via
// mongos and via mongod, if started with --configsvr
(function() {
    "use strict";

    var getConfigsvrToWriteTo = function(st) {
        if (st.configRS) {
            return st.configRS.getPrimary();
        } else {
            return st._configServers[0];
        }
    };

    var st = new ShardingTest({shards: 2});
    var mongos = st.s;
    var config = getConfigsvrToWriteTo(st).getDB('config');

    // Try to drop config db via configsvr

    print("1: Try to drop config database via configsvr");
    assert.eq(0, config.dropDatabase().ok);
    assert.eq("Cannot drop 'config' database if mongod started with --configsvr",
              config.dropDatabase().errmsg);

    // Try to drop config db via mongos
    var config = mongos.getDB("config");

    print("1: Try to drop config database via mongos");
    assert.eq(0, config.dropDatabase().ok);

    // 20 = ErrorCodes::IllegalOperation
    assert.eq(20, config.dropDatabase().code);

    st.stop();
}());