/**
 * Tests that setting internalQueryCheckRegexMatchUTF8Boundary=false at startup disables the
 * UTF-8 character-boundary check, so a regex such as \C that crosses a UTF-8 character boundary
 * is no longer rejected with error 12407700.
 *
 * Only operators with boolean match semantics ($regexMatch and the $regex query predicate) are
 * exercised. $regexFind and $regexFindAll advance over the match by code point, so a
 * boundary-crossing match trips a downstream invariant; the boundary check exists to prevent
 * exactly that, which is why disabling it is unsafe for those operators.
 */
const conn = MongoRunner.runMongod({
    setParameter: {internalQueryCheckRegexMatchUTF8Boundary: false},
});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());
const coll = db.regex_utf8_boundary_flag_disabled;
coll.drop();
assert.commandWorked(coll.insertOne({_id: 0, s: "é"}));

// Under the default (check enabled) these regexes reject the mid-character match with 12407700;
// with the check disabled they match without throwing.
const res = coll.aggregate([{$project: {o: {$regexMatch: {input: "$s", regex: "\\C"}}}}]).toArray();
assert.eq(1, res.length, res);
assert.eq(true, res[0].o, res);

assert.eq(1, coll.find({s: {$regex: "\\C"}}).itcount());

MongoRunner.stopMongod(conn);
