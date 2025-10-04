// Test Symbol.toPrimitive works for DB and BSON objects
//
// @tags: [
//   no_selinux,
//   # TODO SERVER-77024: Shell collection creation are broken when name is a js property identifier
//   assumes_no_implicit_collection_creation_on_get_collection
// ]

// Exercise Symbol.toPrimitive on DB objects
assert(`${db}` === "test");
assert(isNaN(+db));

// Exercise the special Symbol methods and make sure DB.getProperty handles them
assert(db[Symbol.iterator] != 1);
assert(db[Symbol.match] != 1);
assert(db[Symbol.species] != 1);
assert(db[Symbol.toPrimitive] != 1);

// Exercise Symbol.toPrimitive on BSON objects
let col1 = db.jssymbol_col;
col1.insert({});
let a = db.getCollection("jssymbol_col").getIndexes()[0];

assert(isNaN(+a));
assert(+a.v >= 1);
assert(`${a.v}` >= 1);
assert(`${a}` == "[object BSON]");

// Exercise the special Symbol methods and make sure BSON.resolve handles them
assert(db[Symbol.iterator] != 1);
assert(db[Symbol.match] != 1);
assert(db[Symbol.species] != 1);
assert(db[Symbol.toPrimitive] != 1);

col1.drop();
