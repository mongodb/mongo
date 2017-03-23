/* test using lots of indexes on one collection */

t = db.many;

function f() {
    t.drop();
    db.many2.drop();

    t.save({x: 9, y: 99});
    t.save({x: 19, y: 99});

    x = 2;
    var lastErr = null;
    while (x < 70) {
        patt = {};
        patt[x] = 1;
        if (x == 20)
            patt = {x: 1};
        if (x == 64)
            patt = {y: 1};
        lastErr = t.ensureIndex(patt);
        x++;
    }

    assert.commandFailed(lastErr, "should have got an error 'too many indexes'");

    // 40 is the limit currently
    lim = t.getIndexes().length;
    if (lim != 64) {
        print("# of indexes should be 64 but is : " + lim);
        return;
    }
    assert(lim == 64, "not 64 indexes");

    assert(t.find({x: 9}).length() == 1, "b");

    assert(t.find({y: 99}).length() == 2, "y idx");

    /* check that renamecollection remaps all the indexes right */
    assert(t.renameCollection("many2").ok, "rename failed");
    assert(t.find({x: 9}).length() == 0, "many2a");
    assert(db.many2.find({x: 9}).length() == 1, "many2b");
    assert(t.find({y: 99}).length() == 0, "many2c");
    assert(db.many2.find({y: 99}).length() == 2, "many2d");
}

f();
