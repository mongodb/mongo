/**
 * Test cloneCollectionAsCapped
 *
 * @tags: [
 *  # The test runs commands that are not allowed with security token: cloneCollectionAsCapped,
 *  # convertToCapped.
 *  not_allowed_with_signed_security_token,
 *  requires_non_retryable_commands,
 *  requires_fastcount,
 *  requires_capped,
 *  # capped collections connot be sharded
 *  assumes_unsharded_collection,
 *  # cloneCollectionAsCapped command is not supported on mongos
 *  # TODO: SERVER-85773 Remove assumes_against_mongod_not_mongos tag
 *  assumes_against_mongod_not_mongos,
 * ]
 */

let source = db.capped_convertToCapped1;
let dest = db.capped_convertToCapped1_clone;

source.drop();
dest.drop();

const numInitialDocs = 1000;
for (let i = 0; i < numInitialDocs; ++i) {
    source.save({i: i});
}
assert.eq(numInitialDocs, source.count());
assert(!source.isCapped());
assert(!dest.isCapped());

// should all fit
assert.commandWorked(db.runCommand(
    {cloneCollectionAsCapped: source.getName(), toCollection: dest.getName(), size: 100000}));
assert(!source.isCapped());
assert(dest.isCapped());
assert.eq(numInitialDocs, dest.count());
assert.eq(numInitialDocs, source.count());

dest.drop();

// should NOT all fit
assert(!dest.isCapped());
assert.commandWorked(db.runCommand(
    {cloneCollectionAsCapped: source.getName(), toCollection: dest.getName(), size: 1000}));
assert(!source.isCapped());
assert(dest.isCapped());

assert.eq(numInitialDocs, source.count());
assert.gt(numInitialDocs, dest.count());
