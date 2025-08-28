// test for SERVER-5013
// make sure very long long lines get truncated

const options = {
    setParameter: "maxLogSizeKB=9",
};
const conn = MongoRunner.runMongod(options);

var db = conn.getDB("db");
var res = db.adminCommand({getParameter: 1, maxLogSizeKB: 1});
assert.eq(9, res.maxLogSizeKB);

let t = db.loglong;
t.drop();

t.insert({x: 1});

let n = 0;
let query = {x: []};
while (Object.bsonsize(query) < 30000) {
    query.x.push(n++);
}

assertLogTruncated(db, t);

var res = db.adminCommand({setParameter: 1, maxLogSizeKB: 8});
assert.eq(res.ok, 1);

assertLogTruncated(db, t);

function assertLogTruncated(db, t) {
    let before = db.adminCommand({setParameter: 1, logLevel: 1});

    t.findOne(query);

    let x = db.adminCommand({setParameter: 1, logLevel: before.was});
    assert.eq(1, x.was, tojson(x));

    let log = db.adminCommand({getLog: "global"}).log;

    let found = false;
    for (let i = log.length - 1; i >= 0; i--) {
        let obj = JSON.parse(log[i]);
        if (obj.hasOwnProperty("truncated")) {
            found = true;
            break;
        }
    }

    assert(found, tojson(log));
}
MongoRunner.stopMongod(conn);
