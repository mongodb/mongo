/**
 * Ensure that the shell correctly handles exhaust queries
 */

const coll = db.shell_exhaust_queries;

// Ensure that read concern is not allowed
db.getMongo().setReadConcern('majority');
assert.throws(() => coll.find().addOption(DBQuery.Option.exhaust).itcount());
db.getMongo().setReadConcern(null);

// Ensure that collation is not allowed
assert.throws(
    () => coll.find().collation({locale: "simple"}).addOption(DBQuery.Option.exhaust).itcount());

// Ensure that "allowDiskUse" is not allowed
assert.throws(() => coll.find().allowDiskUse(true).addOption(DBQuery.Option.exhaust).itcount());

// Ensure that read preference is handled correctly
db.getMongo().setReadPref('secondary');
assert.eq(0, coll.find().addOption(DBQuery.Option.exhaust).itcount());
