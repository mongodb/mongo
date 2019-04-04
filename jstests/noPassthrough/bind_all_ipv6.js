// Startup with --bind_ip_all and --ipv6 should not fail with address already in use.

(function() {
    'use strict';

    const merizo = MerizoRunner.runMerizod({ipv6: "", bind_ip_all: ""});
    assert(merizo !== null, "Database is not running");
    assert.commandWorked(merizo.getDB("test").isMaster(), "isMaster failed");
    MerizoRunner.stopMerizod(merizo);
}());
