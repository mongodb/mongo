// Log bound addresses at startup.

(function() {
    'use strict';

    const mongo = MongoRunner.runMongod({ipv6: '', bind_ip: 'localhost', useLogFiles: true});
    assert.neq(mongo, null, "Database is not running");
    const log = cat(mongo.fullOptions.logFile);
    print(log);
    assert(log.includes('Listening on 127.0.0.1'), "Not listening on AF_INET");
    if (!_isWindows()) {
        assert(log.match(/Listening on .*\.sock/), "Not listening on AF_UNIX");
    }
    MongoRunner.stopMongod(mongo);
}());
