// Test Symbol.toPrimitive works for DB and BSON objects
//
(function() {
    // Exercise Symbol.toPrimitive on DB objects
    assert(`${db}` === 'test');
    assert(isNaN(+db));

    // Exercise the special Symbol methods and make sure DB.getProperty handles them
    assert(db[Symbol.iterator] != 1);
    assert(db[Symbol.match] != 1);
    assert(db[Symbol.species] != 1);
    assert(db[Symbol.toPrimitive] != 1);

    // Exercise Symbol.toPrimitive on BSON objects
    col1 = db.jssymbol_col;
    col1.insert({});
    a = db.getCollection("jssymbol_col").getIndexes()[0];

    assert(isNaN(+a));
    assert(+a.v >= 1);
    assert(`${a.v}` >= 1);
    assert(`${a}` == '[object BSON]');

    // Exercise the special Symbol methods and make sure BSON.resolve handles them
    assert(db[Symbol.iterator] != 1);
    assert(db[Symbol.match] != 1);
    assert(db[Symbol.species] != 1);
    assert(db[Symbol.toPrimitive] != 1);

    col1.drop();
})();
