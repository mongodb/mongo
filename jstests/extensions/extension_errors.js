/**
 * Tests that an extension that uasserts or errors at parse time.
 *
 * Note that this test does not trigger any tasserts to prevent test suite failures.
 * That path will be covered in unit tests which can gracefully handle death tests.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));

function runAssert(assertArgs) {
    return db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$assert: assertArgs}],
        cursor: {},
    });
}

function checkAssertionFailure({errmsg, code, assertionType}) {
    const res = assert.commandFailedWithCode(
        runAssert({
            errmsg,
            code,
            assertionType,
        }),
        code,
    );
    assert.eq(res.errmsg, "Extension encountered error: " + errmsg, res);
    return res;
}

{
    const res = checkAssertionFailure({errmsg: "this is a fake uassert", code: 1234, assertionType: "uassert"});
    assert.eq(res.codeName, "Location1234", res);
}

{
    const res = checkAssertionFailure({
        errmsg: "this is a fake BadValue",
        code: ErrorCodes.BadValue,
        assertionType: "uassert",
    });
    assert.eq(res.codeName, "BadValue", res);
}

{
    // The extension throws a generic runtime error. We are not able to propagate much information from it, which
    // is acceptable because this is not an intended use case. We test it here to document the behavior.
    const res = assert.commandFailed(
        runAssert({
            assertionType: "error",
        }),
    );
    assert.eq(res.errmsg, "", res);
    assert.eq(res.code, -1, res);
}
