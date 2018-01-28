// Startup with --bind_ip_all and --ipv6 should not fail with address already in use.

(function() {
    'use strict';

    const mongo = MongoRunner.runMongod({ipv6: "", bind_ip_all: ""});
    assert(mongo !== null, "Database is not running");
    assert.commandWorked(mongo.getDB("test").isMaster(), "isMaster failed");
    MongoRunner.stopMongod(mongo);
}());
