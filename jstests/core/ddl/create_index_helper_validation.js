// Tests that helper functions for creating indexes check the number of arguments passed.

const coll = db.create_index_helper_validation;
coll.drop();

assert.throws(() => coll.createIndexes(
                  /* keys */[{a: 1}],
                  /* options */ {},
                  /* commitQuorum */ "majority",
                  {unique: true}));

assert.throws(() => coll.createIndex(
                  /* keys */ {a: 1},
                  /* options */ {},
                  /* commitQuorum */ "majority",
                  {unique: true}));

assert.throws(() => coll.createIndex(
                  /* keys */ {a: 1},
                  /* options */ {},
                  /* commitQuorum */ "majority",
                  {unique: true}));