// SERVER-8625: Test that dbAdmins can view index definitions.
let conn = MongoRunner.runMongod({auth: ""});

let adminDB = conn.getDB("admin");
let testDB = conn.getDB("testdb");
let indexName = "idx_a";

adminDB.createUser({user: "root", pwd: "password", roles: ["root"]});
adminDB.auth("root", "password");
testDB.foo.insert({a: 1});
testDB.createUser({user: "dbAdmin", pwd: "password", roles: ["dbAdmin"]});
adminDB.logout();

testDB.auth("dbAdmin", "password");
testDB.foo.createIndex({a: 1}, {name: indexName});
assert.eq(2, testDB.foo.getIndexes().length); // index on 'a' plus default _id index
let indexList = testDB.foo.getIndexes().filter(function (idx) {
    return idx.name === indexName;
});
assert.eq(1, indexList.length, tojson(indexList));
assert.docEq(indexList[0].key, {a: 1}, tojson(indexList));
MongoRunner.stopMongod(conn, null, {user: "root", pwd: "password"});
