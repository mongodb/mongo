f = db.jstests_update4;
f.drop();

getLastError = function() {
    ret = db.runCommand({getlasterror: 1});
    //    printjson( ret );
    return ret;
};

f.save({a: 1});
f.update({a: 1}, {a: 2});
assert.eq(true, getLastError().updatedExisting, "A");
assert.eq(1, getLastError().n, "B");
f.update({a: 1}, {a: 2});
assert.eq(false, getLastError().updatedExisting, "C");
assert.eq(0, getLastError().n, "D");

f.update({a: 1}, {a: 1}, true);
assert.eq(false, getLastError().updatedExisting, "E");
assert.eq(1, getLastError().n, "F");
f.update({a: 1}, {a: 1}, true);
assert.eq(true, getLastError().updatedExisting, "G");
assert.eq(1, getLastError().n, "H");
assert.eq(true, db.getPrevError().updatedExisting, "I");
assert.eq(1, db.getPrevError().nPrev, "J");

f.findOne();
assert.eq(undefined, getLastError().updatedExisting, "K");
assert.eq(true, db.getPrevError().updatedExisting, "L");
assert.eq(2, db.getPrevError().nPrev, "M");

db.forceError();
assert.eq(undefined, getLastError().updatedExisting, "N");
