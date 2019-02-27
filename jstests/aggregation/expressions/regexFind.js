/*
 * Tests for $regexFind aggregation expression.
 */
(function() {
    'use strict';

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode().

    const coll = db.regex_find_expr;
    coll.drop();

    function testRegexFindAgg(regexFind, expectedOutput) {
        const result =
            coll.aggregate([
                    {"$project": {_id: 0, "matches": {"$regexFind": regexFind}}},
                    {"$sort": {"matches": 1}}  // Ensure that the documents are returned in a
                                               // deterministic order for sharded clusters.
                ])
                .toArray();
        assert.eq(result, expectedOutput);
    }
    function testRegexFindAggForKey(key, regexFind, expectedMatchObj) {
        const result = coll.aggregate([
                               {"$match": {"_id": key}},
                               {"$project": {"matches": {"$regexFind": regexFind}}}
                           ])
                           .toArray();
        const expectedOutput = [{"_id": key, "matches": expectedMatchObj}];
        assert.eq(result, expectedOutput);
    }
    function testRegexFindAggException(regexFind, exceptionCode) {
        assertErrorCode(
            coll, [{"$project": {"matches": {"$regexFind": regexFind}}}], exceptionCode);
    }

    (function testWithSingleMatch() {
        // Regex in string notation, find with multiple captures.
        assert.commandWorked(coll.insert({_id: 0, text: "Simple Example"}));
        testRegexFindAggForKey(0,
                               {input: "$text", regex: "(m(p))"},
                               {"match": "mp", "idx": 2, "captures": ["mp", "p"]});
        // Regex in json syntax, with multiple captures.
        testRegexFindAggForKey(0, {input: "$text", regex: /(S)(i)(m)(p)(l)(e) (Ex)(am)(p)(le)/}, {
            "match": "Simple Example",
            "idx": 0,
            "captures": ["S", "i", "m", "p", "l", "e", "Ex", "am", "p", "le"]
        });

        // Regex string groups within group.
        testRegexFindAggForKey(
            0,
            {input: "$text", regex: "((S)(i)(m)(p)(l)(e))"},
            {"match": "Simple", "idx": 0, "captures": ["Simple", "S", "i", "m", "p", "l", "e"]});
        testRegexFindAggForKey(
            0,
            {input: "$text", regex: "(S)(i)(m)((p)(l)(e))"},
            {"match": "Simple", "idx": 0, "captures": ["S", "i", "m", "ple", "p", "l", "e"]});

        // Regex email pattern.
        assert.commandWorked(
            coll.insert({_id: 1, text: "Some field text with email mongo@mongodb.com"}));
        testRegexFindAggForKey(
            1,
            {input: "$text", regex: "([a-zA-Z0-9._-]+)@[a-zA-Z0-9._-]+\.[a-zA-Z0-9._-]+"},
            {"match": "mongo@mongodb.com", "idx": 27, "captures": ["mongo"]});

        // Regex digits.
        assert.commandWorked(coll.insert({_id: 5, text: "Text with 02 digits"}));
        testRegexFindAggForKey(
            5, {input: "$text", regex: /[0-9]+/}, {"match": "02", "idx": 10, "captures": []});
        testRegexFindAggForKey(
            5, {input: "$text", regex: /(\d+)/}, {"match": "02", "idx": 10, "captures": ["02"]});

        // Regex a non-capture group.
        assert.commandWorked(coll.insert({_id: 6, text: "1,2,3,4,5,6,7,8,9,10"}));
        testRegexFindAggForKey(6,
                               {input: "$text", regex: /^(?:1|a)\,([0-9]+)/},
                               {"match": "1,2", "idx": 0, "captures": ["2"]});

        // Regex quantifier.
        assert.commandWorked(coll.insert({_id: 7, text: "abc12defgh345jklm"}));
        testRegexFindAggForKey(
            7, {input: "$text", regex: /[0-9]{3}/}, {"match": "345", "idx": 10, "captures": []});

        // Regex case insensitive option.
        assert.commandWorked(coll.insert({_id: 8, text: "This Is Camel Case"}));
        testRegexFindAggForKey(8, {input: "$text", regex: /camel/}, null);
        testRegexFindAggForKey(
            8, {input: "$text", regex: /camel/i}, {"match": "Camel", "idx": 8, "captures": []});
        testRegexFindAggForKey(8,
                               {input: "$text", regex: /camel/, options: "i"},
                               {"match": "Camel", "idx": 8, "captures": []});
        testRegexFindAggForKey(8,
                               {input: "$text", regex: "camel", options: "i"},
                               {"match": "Camel", "idx": 8, "captures": []});

        // Regex multi line option.
        assert.commandWorked(coll.insert({_id: 9, text: "Foo line1\nFoo line2\nFoo line3"}));
        // Verify no match with options flag off.
        testRegexFindAggForKey(9, {input: "$text", regex: /^Foo line\d$/}, null);
        // Verify match when flag is on.
        testRegexFindAggForKey(9,
                               {input: "$text", regex: /^Foo line\d$/m},
                               {"match": "Foo line1", "idx": 0, "captures": []});

        // Regex single line option.
        testRegexFindAggForKey(9,
                               {input: "$text", regex: "Foo.*line"},
                               {"match": "Foo line", "idx": 0, "captures": []});
        testRegexFindAggForKey(
            9,
            {input: "$text", regex: "Foo.*line", options: "s"},
            {"match": "Foo line1\nFoo line2\nFoo line", "idx": 0, "captures": []});

        // Regex extended option.
        testRegexFindAggForKey(9, {input: "$text", regex: "F o o # a comment"}, null);
        testRegexFindAggForKey(9,
                               {input: "$text", regex: "F o o # a comment", options: "x"},
                               {"match": "Foo", "idx": 0, "captures": []});
        testRegexFindAggForKey(
            9,
            {input: "$text", regex: "F o o # a comment \n\n# ignored", options: "x"},
            {"match": "Foo", "idx": 0, "captures": []});
        testRegexFindAggForKey(9,
                               {input: "$text", regex: "(F o o) # a comment", options: "x"},
                               {"match": "Foo", "idx": 0, "captures": ["Foo"]});

        // Regex pattern from a document field value.
        assert.commandWorked(coll.insert({_id: 10, text: "Simple Value", pattern: "(m(p))"}));
        testRegexFindAggForKey(10,
                               {input: "$text", regex: "$pattern"},
                               {"match": "mp", "idx": 2, "captures": ["mp", "p"]});
        assert.commandWorked(coll.insert({_id: 11, text: "OtherText", pattern: /(T(e))xt$/}));
        testRegexFindAggForKey(11,
                               {input: "$text", regex: "$pattern"},
                               {"match": "Text", "idx": 5, "captures": ["Te", "e"]});

        // 'regex' as object with null characters.
        assert.commandWorked(coll.insert({_id: 12, text: "Null\0 charac\0ters"}));
        testRegexFindAggForKey(12, {input: "$text", regex: /((Null)(\0))( )(charac\0t)/}, {
            "match": "Null\0 charac\0t",
            "idx": 0,
            "captures": ["Null\0", "Null", "\0", " ", "charac\0t"]
        });
        testRegexFindAggForKey(
            12,
            {input: "$text", regex: /(\x00)( )(charac\x00t)/},
            {"match": "\0 charac\x00t", "idx": 4, "captures": ["\x00", " ", "charac\0t"]});
        // 'regex' as string with escaped null characters.
        testRegexFindAggForKey(12,
                               {input: "$text", regex: "l\\0 charac\\0ter.*$"},
                               {"match": "l\0 charac\0ters", "idx": 3, "captures": []});
        // No match with null characters in input.
        testRegexFindAggForKey(12, {input: "$text", regex: /Null c/}, null);
        // No match with null characters in regex.
        testRegexFindAggForKey(12, {input: "$text", regex: /Nul\0l/}, null);

        // No matches.
        testRegexFindAggForKey(0, {input: "$text", regex: /foo/}, null);
        // Regex null.
        testRegexFindAggForKey(0, {input: "$text", regex: null}, null);
        // Regex not present.
        testRegexFindAggForKey(0, {input: "$text"}, null);
        // Input not present.
        testRegexFindAggForKey(0, {regex: /valid/}, null);
        // Input null.
        testRegexFindAggForKey(0, {input: null, regex: /valid/}, null);
        // Empty object.
        testRegexFindAggForKey(0, {}, null);
    })();

    (function testWithStartOptions() {
        coll.drop();
        assert.commandWorked(coll.insert({_id: 2, text: "cafétéria"}));
        assert.commandWorked(coll.insert({_id: 3, text: "ab\ncd"}));

        // LIMIT_MATCH option to limit the number of comparisons PCRE does internally.
        testRegexFindAggForKey(2, {input: "$text", regex: "(*LIMIT_MATCH=1)fé"}, null);
        testRegexFindAggForKey(2,
                               {input: "$text", regex: "(*LIMIT_MATCH=3)(fé)"},
                               {"match": "fé", "idx": 2, "captures": ["fé"]});

        // (*LF) will change the feed system to UNIX like and (*CR) to windows like. So '\n' would
        // match '.' with CR but not LF.
        testRegexFindAggForKey(3, {input: "$text", regex: "(*LF)ab.cd"}, null);
        testRegexFindAggForKey(3,
                               {input: "$text", regex: "(*CR)ab.cd"},
                               {"match": "ab\ncd", "idx": 0, "captures": []});

        // Multiple start options.
        testRegexFindAggForKey(2,
                               {input: "$text", regex: String.raw `(*LIMIT_MATCH=5)(*UCP)^(\w+)`},
                               {"match": "cafétéria", "idx": 0, "captures": ["cafétéria"]});
        testRegexFindAggForKey(
            2, {input: "$text", regex: String.raw `(*LIMIT_MATCH=1)(*UCP)^(\w+)`}, null);
    })();

    (function testWithUnicodeData() {
        coll.drop();
        // Unicode index counting.
        assert.commandWorked(coll.insert({_id: 2, text: "cafétéria"}));
        assert.commandWorked(coll.insert({_id: 3, text: "मा०गो डीबि"}));
        testRegexFindAggForKey(
            2, {input: "$text", regex: "té"}, {"match": "té", "idx": 4, "captures": []});
        testRegexFindAggForKey(
            3, {input: "$text", regex: /म/}, {"match": "म", "idx": 0, "captures": []});
        // Unicode with capture group.
        testRegexFindAggForKey(3,
                               {input: "$text", regex: /(गो )/},
                               {"match": "गो ", "idx": 3, "captures": ["गो "]});
        // Test that regexes support Unicode character properties.
        testRegexFindAggForKey(2, {input: "$text", regex: String.raw `\p{Hangul}`}, null);
        testRegexFindAggForKey(2,
                               {input: "$text", regex: String.raw `\p{Latin}+$`},
                               {"match": "cafétéria", "idx": 0, "captures": []});
        // Test that the (*UTF) and (*UTF8) options are accepted for unicode characters.
        assert.commandWorked(coll.insert({_id: 12, text: "༢༣༤༤༤༥12༥A"}));
        testRegexFindAggForKey(
            12, {input: "$text", regex: "(*UTF8)༤"}, {"match": "༤", "idx": 2, "captures": []});
        testRegexFindAggForKey(
            12, {input: "$text", regex: "(*UTF)༤"}, {"match": "༤", "idx": 2, "captures": []});
        // For ASCII characters.
        assert.commandWorked(coll.insert({_id: 4, text: "123444"}));
        testRegexFindAggForKey(4,
                               {input: "$text", regex: "(*UTF8)(44)"},
                               {"match": "44", "idx": 3, "captures": ["44"]});
        testRegexFindAggForKey(4,
                               {input: "$text", regex: "(*UTF)(44)"},
                               {"match": "44", "idx": 3, "captures": ["44"]});

        // When the (*UCP) option is specified, Unicode "word" characters are included in the '\w'
        // character type.
        testRegexFindAggForKey(12,
                               {input: "$text", regex: String.raw `(*UCP)^(\w+)`},
                               {"match": "༢༣༤༤༤༥12༥A", "idx": 0, "captures": ["༢༣༤༤༤༥12༥A"]});
        // When the (*UCP) option is specified, [:digit:] becomes \p{N} and matches all Unicode
        // decimal digit characters.
        testRegexFindAggForKey(12,
                               {input: "$text", regex: "(*UCP)^[[:digit:]]+"},
                               {"match": "༢༣༤༤༤༥12༥", "idx": 0, "captures": []});
        testRegexFindAggForKey(12, {input: "$text", regex: "(*UCP)[[:digit:]]+$"}, null);
        // When the (*UCP) option is specified, [:alpha:] becomes \p{L} and matches all Unicode
        // alphabetic characters.
        assert.commandWorked(coll.insert({_id: 13, text: "박정수AB"}));
        testRegexFindAggForKey(13,
                               {input: "$text", regex: String.raw `(*UCP)^[[:alpha:]]+`},
                               {"match": "박정수AB", "idx": 0, "captures": []});

        // No match when options are not set.
        testRegexFindAggForKey(12, {input: "$text", regex: String.raw `^(\w+)`}, null);
        testRegexFindAggForKey(12, {input: "$text", regex: "^[[:digit:]]"}, null);
        testRegexFindAggForKey(2, {input: "$text", regex: "^[[:alpha:]]+$"}, null);
    })();

    (function testErrors() {
        coll.drop();
        assert.commandWorked(coll.insert({text: "string"}));
        // Null object.
        testRegexFindAggException(null, 51103);
        // Incorrect object parameter.
        testRegexFindAggException("incorrect type", 51103);
        // Test malformed regex.
        testRegexFindAggException({input: "$text", regex: "[0-9"}, 51111);
        // Malformed regex because start options not at the beginning.
        testRegexFindAggException({input: "$text", regex: "^(*UCP)[[:alpha:]]+$"}, 51111);
        testRegexFindAggException({input: "$text", regex: "((*UCP)[[:alpha:]]+$)"}, 51111);
        // At least one of the 'input' field is not string.
        assert.commandWorked(coll.insert({a: "string"}));
        assert.commandWorked(coll.insert({a: {b: "object"}}));
        testRegexFindAggException({input: "$a", regex: "valid"}, 51104);
        // 'regex' field is not string or regex.
        testRegexFindAggException({input: "$text", regex: ["incorrect"]}, 51105);
        // 'options' field is not string.
        testRegexFindAggException({input: "$text", regex: "valid", options: 123}, 51106);
        // Incorrect 'options' flag.
        testRegexFindAggException({input: "$text", regex: "valid", options: 'a'}, 51108);
        // 'options' are case-sensitive.
        testRegexFindAggException({input: "$text", regex: "valid", options: "I"}, 51108);
        // Options specified in both 'regex' and 'options'.
        testRegexFindAggException({input: "$text", regex: /(m(p))/i, options: "i"}, 51107);
        testRegexFindAggException({input: "$text", regex: /(m(p))/i, options: "x"}, 51107);
        testRegexFindAggException({input: "$text", regex: /(m(p))/m, options: ""}, 51107);
        // 'regex' as string with null characters.
        testRegexFindAggException({input: "$text", regex: "sasd\0", options: "i"}, 51109);
        testRegexFindAggException({input: "$text", regex: "sa\x00sd", options: "i"}, 51109);
        // 'options' as string with null characters.
        testRegexFindAggException({input: "$text", regex: /(m(p))/, options: "i\0"}, 51110);
        testRegexFindAggException({input: "$text", regex: /(m(p))/, options: "i\x00"}, 51110);

    })();

    (function testMultipleMatches() {
        coll.drop();
        assert.commandWorked(coll.insert({a: "string1"}));
        assert.commandWorked(coll.insert({a: "string2"}));
        // Both match.
        testRegexFindAgg({input: "$a", regex: "(^str.*)"}, [
            {"matches": {"match": "string1", "idx": 0, "captures": ["string1"]}},
            {"matches": {"match": "string2", "idx": 0, "captures": ["string2"]}}
        ]);
        // Only one match.
        testRegexFindAgg({input: "$a", regex: "(^.*[0-1]$)"}, [
            {"matches": null},
            {"matches": {"match": "string1", "idx": 0, "captures": ["string1"]}}
        ]);
        // None match.
        testRegexFindAgg({input: "$a", regex: "(^.*[3-9]$)"},
                         [{"matches": null}, {"matches": null}]);
    })();

    (function testInsideCondOperator() {
        coll.drop();
        assert.commandWorked(
            coll.insert({_id: 0, level: "Public Knowledge", info: "Company Name"}));
        assert.commandWorked(
            coll.insert({_id: 1, level: "Private Information", info: "Company Secret"}));

        const result =
            coll.aggregate([{
                    "$project": {
                        "information": {
                            "$cond": [
                                {
                                  "$eq":
                                      [{"$regexFind": {input: "$level", regex: /public/i}}, null]
                                },
                                "REDACTED",
                                "$info"
                            ]
                        }
                    }
                }])
                .toArray();
        assert.eq(result, [
            {"_id": 0, "information": "Company Name"},
            {"_id": 1, "information": "REDACTED"},
        ]);
    })();
}());
