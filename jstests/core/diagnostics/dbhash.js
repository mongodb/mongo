// @tags: [
//   # The test runs commands that are not allowed with security token: dbhash.
//   not_allowed_with_signed_security_token,
// ]

let a = db.dbhasha;
let b = db.dbhashb;

a.drop();
b.drop();

// debug SERVER-761
db.getCollectionNames().forEach(function (x) {
    let v = db[x].validate();
    if (!v.valid) {
        print(x);
        printjson(v);
    }
});

function dbhash(mydb) {
    let ret = mydb.runCommand("dbhash");
    assert.commandWorked(ret, "dbhash failure");
    return ret;
}

function gh(coll, mydb) {
    if (!mydb) mydb = db;
    let x = dbhash(mydb).collections[coll.getName()];
    if (!x) return "";
    return x;
}

function dbh(mydb) {
    return dbhash(mydb).md5;
}

assert.eq(gh(a), gh(b), "A1");

a.insert({_id: 5});
assert.neq(gh(a), gh(b), "A2");

b.insert({_id: 5});
assert.eq(gh(a), gh(b), "A3");

let dba = db.getSiblingDB("dbhasha");
let dbb = db.getSiblingDB("dbhashb");

dba.dropDatabase();
dbb.dropDatabase();

assert.eq(gh(dba.foo, dba), gh(dbb.foo, dbb), "B1");
assert.eq(dbh(dba), dbh(dbb), "C1");

dba.foo.insert({_id: 5});
assert.neq(gh(dba.foo, dba), gh(dbb.foo, dbb), "B2");
assert.neq(dbh(dba), dbh(dbb), "C2");

dbb.foo.insert({_id: 5});
assert.eq(gh(dba.foo, dba), gh(dbb.foo, dbb), "B3");
assert.eq(dbh(dba), dbh(dbb), "C3");

// Validate dbHash with an empty database does not trigger an fassert/invariant.
assert.commandFailed(db.runCommand({"dbhash": ""}));
