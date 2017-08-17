(function() {
    'use strict';

    var proc = MongoRunner.runMongod({bind_ip: "localhost", "ipv6": "", waitForConnect: false});
    assert.neq(proc, null);

    assert.soon(function() {
        try {
            var conn = new Mongo(`mongodb://127.0.0.1:${proc.port}/test?socketTimeoutMS=5000`);
            assert.commandWorked(conn.adminCommand({ping: 1}));
        } catch (e) {
            return false;
        }
        return true;
    }, "Cannot connect to 127.0.0.1 when bound to localhost", 20000);
})();
