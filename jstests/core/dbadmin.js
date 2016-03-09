load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';

    var t = db.dbadmin;
    t.save({x: 1});
    t.save({x: 1});

    var res = db._adminCommand("listDatabases");
    assert(res.databases && res.databases.length > 0, "listDatabases 1 " + tojson(res));

    var now = new Date();
    var x = db._adminCommand("ismaster");
    assert(x.ismaster, "ismaster failed: " + tojson(x));
    assert(x.localTime, "ismaster didn't include time: " + tojson(x));

    var localTimeSkew = x.localTime - now;
    if (localTimeSkew >= 50) {
        print("Warning: localTimeSkew " + localTimeSkew + " > 50ms.");
    }
    assert.lt(localTimeSkew, 500, "isMaster.localTime");

    var before = db.runCommand("serverStatus");
    print(before.uptimeEstimate);
    sleep(5000);

    var after = db.runCommand("serverStatus");
    print(after.uptimeEstimate);
    assert.gte(
        after.uptimeEstimate, before.uptimeEstimate, "uptime estimate should be non-decreasing");

})();
