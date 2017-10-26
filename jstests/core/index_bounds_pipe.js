/**
 * Tests the tightness of index bounds when attempting to match a regex that contains escaped and
 * non-escaped pipe '|' characters.
 */
(function() {
    'use strict';

    load('jstests/libs/analyze_plan.js');

    const collName = 'index_bounds_pipe';
    const coll = db.getCollection(collName);
    coll.drop();

    assert.writeOK(coll.insert({_id: ''}));
    assert.writeOK(coll.insert({_id: '\\|'}));
    assert.writeOK(coll.insert({_id: 'a'}));
    assert.writeOK(coll.insert({_id: 'a|b'}));
    assert.writeOK(coll.insert({_id: 'b'}));
    assert.writeOK(coll.insert({_id: '|'}));

    /**
     * Asserts that a query on a field using 'params.regex' uses index bounds 'params.bounds' and
     * returns results identical to 'params.results'.
     *
     * Also tests that a query using 'params.regex' will return documents with a field of type regex
     * with an identical regular expression value.
     */
    function assertIndexBoundsAndResult(params) {
        const query = {_id: params.regex};
        const command = {find: collName, filter: query, projection: {_id: 1}, sort: {_id: 1}};
        const explain = db.runCommand({explain: command});
        assert.commandWorked(explain);

        // Check that the query uses correct index bounds.
        const ixscan = getPlanStage(explain.queryPlanner.winningPlan, 'IXSCAN');
        assert.neq(ixscan, null, 'Plan unexpectedly missing IXSCAN stage: ' + tojson(explain));
        assert.eq(ixscan.indexBounds._id,
                  params.bounds,
                  'Expected bounds of ' + tojson(params.bounds) + ' but got ' +
                      tojson(ixscan.indexBounds._id));

        // Check that the query regex matches expected strings.
        const results = db.runCommand(command);
        assert.commandWorked(results);
        assert.eq(results.cursor.firstBatch,
                  params.results,
                  'Regex query ' + tojson(query) + ' returned incorrect results');

        // Check that the query regex will exactly match identical regular expression objects.
        const collRegexValue = db.getCollection(collName + params.regex);
        collRegexValue.drop();
        assert.commandWorked(collRegexValue.createIndex({x: 1}));

        const doc = {_id: 0, x: params.regex};
        assert.writeOK(collRegexValue.insert(doc));

        const regexQuery = {x: params.regex};
        assert.eq(collRegexValue.findOne(regexQuery),
                  doc,
                  'Regex query ' + tojson(regexQuery) +
                      ' did not match document with identical regex value');
    }

    // An anchored regex that uses no special operators can use tight index bounds.
    assertIndexBoundsAndResult(
        {regex: /^a/, bounds: ['["a", "b")', '[/^a/, /^a/]'], results: [{_id: 'a'}, {_id: 'a|b'}]});
    assertIndexBoundsAndResult(
        {regex: /^\\/, bounds: ['["\\", "]")', '[/^\\\\/, /^\\\\/]'], results: [{_id: '\\|'}]});

    // An anchored regex using the alternation operator cannot use tight index bounds.
    assertIndexBoundsAndResult({
        regex: /^a|b/,
        bounds: ['["", {})', '[/^a|b/, /^a|b/]'],
        results: [{_id: 'a'}, {_id: 'a|b'}, {_id: 'b'}]
    });

    // An anchored regex that uses an escaped pipe character can use tight index bounds.
    assertIndexBoundsAndResult(
        {regex: /^a\|/, bounds: ['["a|", "a}")', '[/^a\\|/, /^a\\|/]'], results: [{_id: 'a|b'}]});
    assertIndexBoundsAndResult(
        {regex: /^\|/, bounds: ['["|", "}")', '[/^\\|/, /^\\|/]'], results: [{_id: '|'}]});

    // A pipe character that is preceded by an escaped backslash is correctly interpreted as the
    // alternation operator and cannot use tight index bounds.
    assertIndexBoundsAndResult({
        regex: /^\\|b/,
        bounds: ['["", {})', '[/^\\\\|b/, /^\\\\|b/]'],
        results: [{_id: '\\|'}, {_id: 'a|b'}, {_id: 'b'}]
    });
    assertIndexBoundsAndResult({
        regex: /^\\|^b/,
        bounds: ['["", {})', '[/^\\\\|^b/, /^\\\\|^b/]'],
        results: [{_id: '\\|'}, {_id: 'b'}]
    });

    // An escaped backslash immediately followed by an escaped pipe does not use tight index bounds.
    assertIndexBoundsAndResult({
        regex: /^\\\|/,
        bounds: ['["", {})', '[/^\\\\\\|/, /^\\\\\\|/]'],
        results: [{_id: '\\|'}]
    });

    // A pipe escaped with the \Q...\E escape sequence does not use tight index bounds.
    assertIndexBoundsAndResult(
        {regex: /^\Q|\E/, bounds: ['["", {})', '[/^\\Q|\\E/, /^\\Q|\\E/]'], results: [{_id: '|'}]});

    // An escaped pipe within \Q...\E can use tight index bounds.
    assertIndexBoundsAndResult({
        regex: /^\Q\|\E/,
        bounds: ['["\\|", "\\}")', '[/^\\Q\\|\\E/, /^\\Q\\|\\E/]'],
        results: [{_id: '\\|'}]
    });
}());
