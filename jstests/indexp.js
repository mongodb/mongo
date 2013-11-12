// Tests that SERVER-11374 is fixed: specifically, that indexes cannot
// be created on fields that begin with '$' but are not part of DBRefs
// and that indexes cannot be created on field paths that contain empty
// fields.

var coll = db.jstests_indexp;

// Empty field checks.
coll.ensureIndex({ 'a..b': 1 });
assert( db.getLastError() != null,
        "Expected error, but index on 'a..b' was created successfully." );

coll.ensureIndex({ '.a': 1 });
assert( db.getLastError() != null,
        "Expected error, but index on '.a' was created successfully." );

coll.ensureIndex({ 'a.': 1 });
assert( db.getLastError() != null,
        "Expected error, but index on 'a.' was created successfully." );

coll.ensureIndex({ '.': 1 });
assert( db.getLastError() != null,
        "Expected error, but index on '.' was created successfully." );

coll.ensureIndex({ 'a.b': 1 });
assert( db.getLastError() == null,
        "Expected no error, but creating index on 'a.b' failed." );

// '$'-prefixed field checks.
coll.ensureIndex({ '$a': 1 });
assert( db.getLastError() != null,
        "Expected error, but index on '$a' was created successfully." );

coll.ensureIndex({ 'a.$b': 1 });
assert( db.getLastError() != null,
        "Expected error, but index on 'a.$b' was created successfully." );

coll.ensureIndex({ 'a$ap': 1 });
assert( db.getLastError() == null,
        "Expected no error, but creating index on 'a$ap' failed." );

coll.ensureIndex({ '$db': 1 });
assert( db.getLastError() != null,
        "Expected error, but index on '$db' was created successfully." );

coll.ensureIndex({ 'a.$id': 1 });
assert( db.getLastError() == null,
        "Expected no error, but creating index on 'a.$id' failed." );

coll.dropIndexes();
