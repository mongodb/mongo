/**
 * Test regexes with various Unicode options.
 */
(function() {
    "use strict";

    const coll = db.getCollection("regex_unicode");
    coll.drop();

    // Populate the collection with strings containing ASCII and non-ASCII characters.
    let docAllAscii = {_id: 0, text: "kyle"};
    let docNoAscii = {_id: 1, text: "박정수"};
    let docMixed = {_id: 2, text: "suárez"};
    [docAllAscii, docNoAscii, docMixed].forEach((doc) => assert.writeOK(coll.insert(doc)));

    /**
     * Helper function that asserts that a find command with a filter on the "text" field using
     * 'regex' returns 'expected' when sorting by _id ascending.
     */
    function assertFindResultsEq(regex, expected) {
        const res = coll.find({text: {$regex: regex}}).sort({_id: 1}).toArray();
        const errfn =
            `Regex query "${regex}" returned ${tojson(res)} ` + `but expected ${tojson(expected)}`;
        assert.eq(res, expected, errfn);
    }

    // Sanity check on exact characters.
    assertFindResultsEq("y", [docAllAscii]);
    assertFindResultsEq("e", [docAllAscii, docMixed]);
    assertFindResultsEq("á", [docMixed]);
    assertFindResultsEq("정", [docNoAscii]);

    // Test that the (*UTF) and (*UTF8) options are accepted.
    assertFindResultsEq("(*UTF)e", [docAllAscii, docMixed]);
    assertFindResultsEq("(*UTF)á", [docMixed]);
    assertFindResultsEq("(*UTF)정", [docNoAscii]);
    assertFindResultsEq("(*UTF8)e", [docAllAscii, docMixed]);
    assertFindResultsEq("(*UTF8)á", [docMixed]);
    assertFindResultsEq("(*UTF8)정", [docNoAscii]);

    // Test that regexes support Unicode character properties.
    assertFindResultsEq(String.raw `\p{Latin}`, [docAllAscii, docMixed]);
    assertFindResultsEq(String.raw `^\p{Latin}+$`, [docAllAscii, docMixed]);
    assertFindResultsEq(String.raw `\p{Hangul}`, [docNoAscii]);
    assertFindResultsEq(String.raw `^\p{Hangul}+$`, [docNoAscii]);
    assertFindResultsEq(String.raw `^\p{L}+$`, [docAllAscii, docNoAscii, docMixed]);
    assertFindResultsEq(String.raw `^\p{Xan}+$`, [docAllAscii, docNoAscii, docMixed]);

    // Tests for the '\w' character type, which matches any "word" character. In the default mode,
    // characters outside of the ASCII code point range are excluded.

    // An unanchored regex should match the two documents that contain at least one ASCII character.
    assertFindResultsEq(String.raw `\w`, [docAllAscii, docMixed]);

    // This anchored regex will only match the document with exclusively ASCII characters, since the
    // Unicode character in the mixed document will prevent it from being considered all "word"
    // characters.
    assertFindResultsEq(String.raw `^\w+$`, [docAllAscii]);

    // When the (*UCP) option is specified, Unicode "word" characters are included in the '\w'
    // character type, so all three documents should match.
    assertFindResultsEq(String.raw `(*UCP)\w`, [docAllAscii, docNoAscii, docMixed]);
    assertFindResultsEq(String.raw `(*UCP)^\w+$`, [docAllAscii, docNoAscii, docMixed]);

    // By default, the [:alpha:] character class matches ASCII alphabetic characters.
    assertFindResultsEq("[[:alpha:]]", [docAllAscii, docMixed]);
    assertFindResultsEq("^[[:alpha:]]+$", [docAllAscii]);

    // When the (*UCP) option is specified, [:alpha:] becomes \p{L} and matches all Unicode
    // alphabetic characters.
    assertFindResultsEq("(*UCP)[[:alpha:]]", [docAllAscii, docNoAscii, docMixed]);
    assertFindResultsEq("(*UCP)^[[:alpha:]]+$", [docAllAscii, docNoAscii, docMixed]);

    // Drop the collection and repopulate it with numerical characters.
    coll.drop();
    docAllAscii = {_id: 0, text: "02191996"};
    docNoAscii = {_id: 1, text: "༢༣༤༥"};
    docMixed = {_id: 2, text: "9୩୪୬୯6"};
    [docAllAscii, docNoAscii, docMixed].forEach((doc) => assert.writeOK(coll.insert(doc)));

    // Sanity check on exact characters.
    assertFindResultsEq("1", [docAllAscii]);
    assertFindResultsEq("9", [docAllAscii, docMixed]);
    assertFindResultsEq("୪", [docMixed]);
    assertFindResultsEq("༣", [docNoAscii]);

    // Test that the regexes are matched by the numeric Unicode character property.
    assertFindResultsEq(String.raw `^\p{N}+$`, [docAllAscii, docNoAscii, docMixed]);
    assertFindResultsEq(String.raw `^\p{Xan}+$`, [docAllAscii, docNoAscii, docMixed]);

    // Tests for the '\d' character type, which matches any "digit" character. In the default mode,
    // characters outside of the ASCII code point range are excluded.
    // An unanchored regex should match the two documents that contain at least one ASCII character.
    assertFindResultsEq(String.raw `\d`, [docAllAscii, docMixed]);

    // This anchored regex will only match the document with exclusively ASCII characters, since the
    // Unicode character in the mixed document will prevent it from being considered all "digit"
    // characters.
    assertFindResultsEq(String.raw `^\d+$`, [docAllAscii]);

    // When the (*UCP) option is specified, Unicode "digit" characters are included in the '\d'
    // character type, so all three documents should match.
    assertFindResultsEq(String.raw `(*UCP)\d`, [docAllAscii, docNoAscii, docMixed]);
    assertFindResultsEq(String.raw `(*UCP)^\d+$`, [docAllAscii, docNoAscii, docMixed]);

    // By default, the [:digit:] character class matches ASCII decimal digit characters.
    assertFindResultsEq("[[:digit:]]", [docAllAscii, docMixed]);
    assertFindResultsEq("^[[:digit:]]+$", [docAllAscii]);

    // When the (*UCP) option is specified, [:digit:] becomes \p{N} and matches all Unicode
    // decimal digit characters.
    assertFindResultsEq("(*UCP)[[:digit:]]", [docAllAscii, docNoAscii, docMixed]);
    assertFindResultsEq("(*UCP)^[[:digit:]]+$", [docAllAscii, docNoAscii, docMixed]);
}());
