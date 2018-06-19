/**
 * Tests regular expressions and the use of various UCP verbs.
 */
(function() {
    "use strict";

    const coll = db.getCollection("regex_backtracking_verbs");
    coll.drop();

    const docA = {_id: 0, text: "a"};
    const docB = {_id: 1, text: "b"};
    [docA, docB].forEach(doc => assert.commandWorked(coll.insert(doc)));

    /**
     * Helper function that asserts that a find command with a filter on the "text" field using
     * 'regex' returns 'expected' when sorting by _id ascending.
     */
    function assertFindResultsEq(regex, expected) {
        const res = coll.find({text: {$regex: regex}}).sort({_id: 1}).toArray();
        const errfn = `Regex query ${tojson(regex)} returned ${tojson(res)} ` +
            `but expected ${tojson(expected)}`;
        assert.eq(res, expected, errfn);
    }

    const assertMatchesEverything = (regex) => assertFindResultsEq(regex, [docA, docB]);
    const assertMatchesNothing = (regex) => assertFindResultsEq(regex, []);

    // On encountering FAIL, the pattern immediately does not match.
    assertMatchesNothing("(*FAIL)");
    assertMatchesNothing("a(*FAIL)");
    assertMatchesNothing("(*FAIL)b");

    // On encountering ACCEPT, the pattern immediately matches.
    assertMatchesEverything("(*ACCEPT)");
    assertMatchesEverything("(*ACCEPT)a");
    assertMatchesEverything("(*ACCEPT)c");
    assertFindResultsEq("b(*ACCEPT)", [docB]);

    // The following tests simply assert that the backtracking verbs are accepted and do not
    // influence matching.
    ["COMMIT", "PRUNE", "PRUNE:FOO", "SKIP", "SKIP:BAR", "THEN", "THEN:BAZ"].forEach(verb => {
        // Verb by itself is the same as an empty regex and matches everything.
        assertMatchesEverything(`(*${verb})`);

        // Verb with pattern does not affect the "matchiness" of the pattern.
        assertFindResultsEq(`(*${verb})a`, [docA]);
        assertFindResultsEq(`(*${verb})[Bb]`, [docB]);
    });
}());
