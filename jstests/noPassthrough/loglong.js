// test for SERVER-5013
// make sure very long long lines get truncated

(function() {
"use strict";

const options = {
    setParameter: "maxLogSizeKB=9"
};
const conn = MongoRunner.runMongod(options);

var db = conn.getDB('db');
var res = db.adminCommand({getParameter: 1, maxLogSizeKB: 1});
assert.eq(9, res.maxLogSizeKB);

var t = db.loglong;
t.drop();

t.insert({x: 1});

var n = 0;
var query = {x: []};
while (Object.bsonsize(query) < 30000) {
    query.x.push(n++);
}

assertLogTruncated(db, t);

var res = db.adminCommand({setParameter: 1, maxLogSizeKB: 8});
assert.eq(res.ok, 1);

assertLogTruncated(db, t);

function assertLogTruncated(db, t) {
    var before = db.adminCommand({setParameter: 1, logLevel: 1});

    t.findOne(query);

    var x = db.adminCommand({setParameter: 1, logLevel: before.was});
    assert.eq(1, x.was, tojson(x));

    var log = db.adminCommand({getLog: "global"}).log;

    var found = false;
    for (var i = log.length - 1; i >= 0; i--) {
        var obj = JSON.parse(log[i]);
        if (obj.hasOwnProperty("truncated")) {
            found = true;
            break;
        }
    }

    assert(found, tojson(log));
}
MongoRunner.stopMongod(conn);
})();
