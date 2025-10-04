/**
 * Tests for $regexFind, $regexFindAll and $regexMatch aggregation expressions.
 */
import "jstests/libs/query/sbe_assert_error_override.js";

import {anyEq, assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.regex_find_expr;
coll.drop();

function testRegex(expression, inputObj, expectedOutput) {
    const result = coll
        .aggregate([
            {"$project": {_id: 0, "matches": {[expression]: inputObj}}},
            {"$sort": {"matches": 1}}, // Sort to ensure the documents are returned in a
            // deterministic order for sharded clusters.
        ])
        .toArray();
    assert.eq(result, expectedOutput);
}
function testRegexForKey(expression, key, inputObj, expectedMatchObj) {
    const result = coll
        .aggregate([{"$match": {"_id": key}}, {"$project": {"matches": {[expression]: inputObj}}}])
        .toArray();
    const expectedOutput = [{"_id": key, "matches": expectedMatchObj}];
    assert.eq(result, expectedOutput);
}

/**
 * This function validates the output against $regexFind, $regexFindAll and $regexMatch
 * expressions.
 */
function testRegexFindAgg(inputObj, expectedOutputForFindAll) {
    testRegex("$regexFindAll", inputObj, expectedOutputForFindAll);
    // For each of the output document, get first element from "matches" array. This will
    // convert 'regexFindAll' output to 'regexFind' output.
    const expectedOutputForFind = expectedOutputForFindAll.map((element) => ({
        matches: element.matches.length == 0 ? null : element.matches[0],
    }));
    testRegex("$regexFind", inputObj, expectedOutputForFind);

    // For each of the output document, if there is at least one element in the array, then
    // there is a match.
    const expectedOutputForMatch = expectedOutputForFindAll.map((element) => ({matches: element.matches.length != 0}));
    testRegex("$regexMatch", inputObj, expectedOutputForMatch);
}

/**
 * This function validates the output against $regexFind, $regexFindAll and $regexMatch
 * expressions.
 */
function testRegexFindAggForKey(key, inputObj, expectedOutputForFindAll) {
    testRegexForKey("$regexFindAll", key, inputObj, expectedOutputForFindAll);

    const expectedOutputForFind = expectedOutputForFindAll.length == 0 ? null : expectedOutputForFindAll[0];
    testRegexForKey("$regexFind", key, inputObj, expectedOutputForFind);

    const expectedOutputForMatch = expectedOutputForFindAll.length != 0;
    testRegexForKey("$regexMatch", key, inputObj, expectedOutputForMatch);
}

/**
 * This function validates the output against $regexFind, $regexFindAll and $regexMatch
 * expressions.
 */
function testRegexAggException(inputObj, exceptionCode) {
    assertErrorCode(coll, [{"$project": {"matches": {"$regexFindAll": inputObj}}}], exceptionCode);
    assertErrorCode(coll, [{"$project": {"matches": {"$regexFind": inputObj}}}], exceptionCode);
    assertErrorCode(coll, [{"$project": {"matches": {"$regexMatch": inputObj}}}], exceptionCode);
}

(function testWithSingleMatch() {
    // Regex in string notation, find with multiple captures and matches.
    assert.commandWorked(coll.insert({_id: 0, text: "Simple Example "}));
    testRegexFindAggForKey(0, {input: "$text", regex: "(m(p))"}, [
        {"match": "mp", "idx": 2, "captures": ["mp", "p"]},
        {"match": "mp", "idx": 10, "captures": ["mp", "p"]},
    ]);

    // Regex in json syntax, with multiple captures and matches.
    testRegexFindAggForKey(0, {input: "$text", regex: /(m(p))/}, [
        {"match": "mp", "idx": 2, "captures": ["mp", "p"]},
        {"match": "mp", "idx": 10, "captures": ["mp", "p"]},
    ]);

    // Verify no overlapping match sub-strings.
    assert.commandWorked(coll.insert({_id: 112, text: "aaaaa aaaa"}));
    testRegexFindAggForKey(112, {input: "$text", regex: /(aa)/}, [
        {"match": "aa", "idx": 0, "captures": ["aa"]},
        {"match": "aa", "idx": 2, "captures": ["aa"]},
        {"match": "aa", "idx": 6, "captures": ["aa"]},
        {"match": "aa", "idx": 8, "captures": ["aa"]},
    ]);
    testRegexFindAggForKey(112, {input: "$text", regex: /(aa)+/}, [
        {"match": "aaaa", "idx": 0, "captures": ["aa"]},
        {"match": "aaaa", "idx": 6, "captures": ["aa"]},
    ]);

    // Verify greedy match.
    testRegexFindAggForKey(112, {input: "$text", regex: /(a+)/}, [
        {"match": "aaaaa", "idx": 0, "captures": ["aaaaa"]},
        {"match": "aaaa", "idx": 6, "captures": ["aaaa"]},
    ]);
    testRegexFindAggForKey(112, {input: "$text", regex: /(a)+/}, [
        {"match": "aaaaa", "idx": 0, "captures": ["a"]},
        {"match": "aaaa", "idx": 6, "captures": ["a"]},
    ]);

    // Verify lazy match.
    assert.commandWorked(coll.insert({_id: 113, text: "aaa aa"}));
    testRegexFindAggForKey(113, {input: "$text", regex: /(a+?)/}, [
        {"match": "a", "idx": 0, "captures": ["a"]},
        {"match": "a", "idx": 1, "captures": ["a"]},
        {"match": "a", "idx": 2, "captures": ["a"]},
        {"match": "a", "idx": 4, "captures": ["a"]},
        {"match": "a", "idx": 5, "captures": ["a"]},
    ]);
    testRegexFindAggForKey(113, {input: "$text", regex: /(a*?)/}, [
        {"match": "", "idx": 0, "captures": [""]},
        {"match": "", "idx": 1, "captures": [""]},
        {"match": "", "idx": 2, "captures": [""]},
        {"match": "", "idx": 3, "captures": [""]},
        {"match": "", "idx": 4, "captures": [""]},
        {"match": "", "idx": 5, "captures": [""]},
    ]);

    // Regex string groups within group.
    testRegexFindAggForKey(0, {input: "$text", regex: "((S)(i)(m)(p)(l)(e))"}, [
        {"match": "Simple", "idx": 0, "captures": ["Simple", "S", "i", "m", "p", "l", "e"]},
    ]);
    testRegexFindAggForKey(0, {input: "$text", regex: "(S)(i)(m)((p)(l)(e))"}, [
        {"match": "Simple", "idx": 0, "captures": ["S", "i", "m", "ple", "p", "l", "e"]},
    ]);

    // Regex email pattern.
    assert.commandWorked(coll.insert({_id: 1, text: "Some field text with email mongo@mongodb.com"}));
    testRegexFindAggForKey(1, {input: "$text", regex: "([a-zA-Z0-9._-]+)@[a-zA-Z0-9._-]+\.[a-zA-Z0-9._-]+"}, [
        {"match": "mongo@mongodb.com", "idx": 27, "captures": ["mongo"]},
    ]);

    // Regex digits.
    assert.commandWorked(coll.insert({_id: 5, text: "Text with 02 digits"}));
    testRegexFindAggForKey(5, {input: "$text", regex: /[0-9]+/}, [{"match": "02", "idx": 10, "captures": []}]);
    testRegexFindAggForKey(5, {input: "$text", regex: /(\d+)/}, [{"match": "02", "idx": 10, "captures": ["02"]}]);

    // Regex a non-capture group.
    assert.commandWorked(coll.insert({_id: 6, text: "1,2,3,4,5,6,7,8,9,10"}));
    testRegexFindAggForKey(6, {input: "$text", regex: /^(?:1|a)\,([0-9]+)/}, [
        {"match": "1,2", "idx": 0, "captures": ["2"]},
    ]);

    assert.commandWorked(coll.insert({_id: "capture_group", text: "ABCDXYZABCDXYZABCDXYZ"}));

    // Matching regex with a non-matching capture group.
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /((Z)|B)(CD)/}, [
        {"match": "BCD", "idx": 1, "captures": ["B", null, "CD"]},
        {"match": "BCD", "idx": 8, "captures": ["B", null, "CD"]},
        {"match": "BCD", "idx": 15, "captures": ["B", null, "CD"]},
    ]);

    // Non-matching regex with a matching capture group.
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /EFG|(BC)E/}, []);
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /(BC)E/}, []);

    // Matching regex with zero matching capture groups because of optional quantifier.
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /BC(P(DE)?)?/}, [
        {"match": "BC", "idx": 1, "captures": [null, null]},
        {"match": "BC", "idx": 8, "captures": [null, null]},
        {"match": "BC", "idx": 15, "captures": [null, null]},
    ]);

    // Matching regex with non-matching capture groups because of optional quantifier.
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /(BC)(P(DE)?)?/}, [
        {"match": "BC", "idx": 1, "captures": ["BC", null, null]},
        {"match": "BC", "idx": 8, "captures": ["BC", null, null]},
        {"match": "BC", "idx": 15, "captures": ["BC", null, null]},
    ]);
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /(ABC)(D(XY)?)?Z/}, [
        {"match": "ABCDXYZ", "idx": 0, "captures": ["ABC", "DXY", "XY"]},
        {"match": "ABCDXYZ", "idx": 7, "captures": ["ABC", "DXY", "XY"]},
        {"match": "ABCDXYZ", "idx": 14, "captures": ["ABC", "DXY", "XY"]},
    ]);

    // Greedy optional quantifier inside a lazy optional quantifier.
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /(ABC)(D(XY)?)??/}, [
        {"match": "ABC", "idx": 0, "captures": ["ABC", null, null]},
        {"match": "ABC", "idx": 7, "captures": ["ABC", null, null]},
        {"match": "ABC", "idx": 14, "captures": ["ABC", null, null]},
    ]);

    // Lazy optional quantifier inside a greedy optional quantifier.
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /(ABC)(D(XY)??)?/}, [
        {"match": "ABCD", "idx": 0, "captures": ["ABC", "D", null]},
        {"match": "ABCD", "idx": 7, "captures": ["ABC", "D", null]},
        {"match": "ABCD", "idx": 14, "captures": ["ABC", "D", null]},
    ]);

    // Everything is optional.
    testRegexFindAggForKey("capture_group", {input: "$text", regex: /(ABC)?(XY)?/}, [
        {"match": "ABC", "idx": 0, "captures": ["ABC", null]},
        // Since everything is optional, every index position in the input is a match.
        {"match": "", "idx": 3, "captures": [null, null]},
        {"match": "XY", "idx": 4, "captures": [null, "XY"]},
        {"match": "", "idx": 6, "captures": [null, null]},
        {"match": "ABC", "idx": 7, "captures": ["ABC", null]},
        {"match": "", "idx": 10, "captures": [null, null]},
        {"match": "XY", "idx": 11, "captures": [null, "XY"]},
        {"match": "", "idx": 13, "captures": [null, null]},
        {"match": "ABC", "idx": 14, "captures": ["ABC", null]},
        {"match": "", "idx": 17, "captures": [null, null]},
        {"match": "XY", "idx": 18, "captures": [null, "XY"]},
        {"match": "", "idx": 20, "captures": [null, null]},
    ]);

    // Regex quantifier.
    assert.commandWorked(coll.insert({_id: 7, text: "abc12defgh345jklm"}));
    testRegexFindAggForKey(7, {input: "$text", regex: /[0-9]{3}/}, [{"match": "345", "idx": 10, "captures": []}]);

    // Regex case insensitive option.
    assert.commandWorked(coll.insert({_id: 8, text: "This Is Camel Case"}));
    testRegexFindAggForKey(8, {input: "$text", regex: /camel/}, []);
    testRegexFindAggForKey(8, {input: "$text", regex: /camel/i}, [{"match": "Camel", "idx": 8, "captures": []}]);
    testRegexFindAggForKey(8, {input: "$text", regex: /camel/, options: "i"}, [
        {"match": "Camel", "idx": 8, "captures": []},
    ]);
    testRegexFindAggForKey(8, {input: "$text", regex: "camel", options: "i"}, [
        {"match": "Camel", "idx": 8, "captures": []},
    ]);

    // Regex multi line option.
    assert.commandWorked(coll.insert({_id: 9, text: "Foo line1\nFoo line2\nFoo line3"}));
    // Verify no match with options flag off.
    testRegexFindAggForKey(9, {input: "$text", regex: /^Foo line\d$/}, []);
    // Verify match when flag is on.
    testRegexFindAggForKey(9, {input: "$text", regex: /(^Foo line\d$)/m}, [
        {"match": "Foo line1", "idx": 0, "captures": ["Foo line1"]},
        {"match": "Foo line2", "idx": 10, "captures": ["Foo line2"]},
        {"match": "Foo line3", "idx": 20, "captures": ["Foo line3"]},
    ]);

    // Regex single line option.
    testRegexFindAggForKey(9, {input: "$text", regex: "Foo.*line"}, [
        {"match": "Foo line", "idx": 0, "captures": []},
        {"match": "Foo line", "idx": 10, "captures": []},
        {"match": "Foo line", "idx": 20, "captures": []},
    ]);
    testRegexFindAggForKey(9, {input: "$text", regex: "Foo.*line", options: "s"}, [
        {"match": "Foo line1\nFoo line2\nFoo line", "idx": 0, "captures": []},
    ]);

    // Regex extended option.
    testRegexFindAggForKey(9, {input: "$text", regex: "F o o # a comment"}, []);
    testRegexFindAggForKey(9, {input: "$text", regex: "F o o # a comment", options: "x"}, [
        {"match": "Foo", "idx": 0, "captures": []},
        {"match": "Foo", "idx": 10, "captures": []},
        {"match": "Foo", "idx": 20, "captures": []},
    ]);
    testRegexFindAggForKey(9, {input: "$text", regex: "F o o # a comment \n\n# ignored", options: "x"}, [
        {"match": "Foo", "idx": 0, "captures": []},
        {"match": "Foo", "idx": 10, "captures": []},
        {"match": "Foo", "idx": 20, "captures": []},
    ]);
    testRegexFindAggForKey(9, {input: "$text", regex: "(F o o) # a comment", options: "x"}, [
        {"match": "Foo", "idx": 0, "captures": ["Foo"]},
        {"match": "Foo", "idx": 10, "captures": ["Foo"]},
        {"match": "Foo", "idx": 20, "captures": ["Foo"]},
    ]);

    // Regex pattern from a document field value.
    assert.commandWorked(coll.insert({_id: 10, text: "Simple Value Example", pattern: "(m(p))"}));
    testRegexFindAggForKey(10, {input: "$text", regex: "$pattern"}, [
        {"match": "mp", "idx": 2, "captures": ["mp", "p"]},
        {"match": "mp", "idx": 16, "captures": ["mp", "p"]},
    ]);
    assert.commandWorked(coll.insert({_id: 11, text: "OtherText", pattern: /(T(e))xt$/}));
    testRegexFindAggForKey(11, {input: "$text", regex: "$pattern"}, [
        {"match": "Text", "idx": 5, "captures": ["Te", "e"]},
    ]);

    assert.commandWorked(coll.insert({_id: "Empty input", text: ""}));

    // Empty input matches empty regex.
    testRegexFindAggForKey("Empty input", {input: "$text", regex: ""}, [{"match": "", "idx": 0, "captures": []}]);

    // Empty capture groups.
    testRegexFindAggForKey("Empty input", {input: "$text", regex: "(missing)|()"}, [
        {"match": "", "idx": 0, "captures": [null, ""]},
    ]);
    testRegexFindAggForKey(0, {input: "bbbb", regex: "()"}, [
        {"match": "", "idx": 0, "captures": [""]},
        {"match": "", "idx": 1, "captures": [""]},
        {"match": "", "idx": 2, "captures": [""]},
        {"match": "", "idx": 3, "captures": [""]},
    ]);

    // No matches.
    testRegexFindAggForKey(0, {input: "$text", regex: /foo/}, []);
    // Regex null.
    testRegexFindAggForKey(0, {input: "$text", regex: null}, []);
    // Input null.
    testRegexFindAggForKey(0, {input: null, regex: /valid/}, []);
    // Both null.
    testRegexFindAggForKey(0, {input: null, regex: null}, []);
    testRegexFindAggForKey(0, {input: "$missingField", regex: "$missingField", options: "i"}, []);
    testRegexFindAggForKey(0, {input: "$missingField", regex: "$$REMOVE", options: "i"}, []);
})();

(function testWithStartOptions() {
    coll.drop();
    assert.commandWorked(coll.insert({_id: 2, text: "cafétéria"}));
    assert.commandWorked(coll.insert({_id: 3, text: "ab\ncd"}));

    // LIMIT_MATCH option to limit the number of comparisons PCRE does internally.
    testRegexAggException({input: "$text", regex: "(*LIMIT_MATCH=1)fé"}, 51156);
    testRegexFindAggForKey(2, {input: "$text", regex: "(*LIMIT_MATCH=3)(fé)"}, [
        {"match": "fé", "idx": 2, "captures": ["fé"]},
    ]);

    // (*LF) would change the feed system to UNIX like and (*CR) to windows like. So '\n' would
    // match '.' with CR but not LF.
    testRegexFindAggForKey(3, {input: "$text", regex: "(*LF)ab.cd"}, []);
    testRegexFindAggForKey(3, {input: "$text", regex: "(*CR)ab.cd"}, [{"match": "ab\ncd", "idx": 0, "captures": []}]);

    // Multiple start options.
    testRegexFindAggForKey(2, {input: "$text", regex: String.raw`(*LIMIT_MATCH=5)(*UCP)^(\w+)`}, [
        {"match": "cafétéria", "idx": 0, "captures": ["cafétéria"]},
    ]);
    testRegexAggException({input: "$text", regex: String.raw`(*LIMIT_MATCH=1)(*UCP)^(\w+)`}, 51156);
})();

(function testWithUnicodeData() {
    coll.drop();
    // Unicode index counting.
    assert.commandWorked(coll.insert({_id: 2, text: "cafétéria"}));
    assert.commandWorked(coll.insert({_id: 3, text: "मा०गो डीबि"}));
    testRegexFindAggForKey(2, {input: "$text", regex: "té"}, [{"match": "té", "idx": 4, "captures": []}]);
    testRegexFindAggForKey(3, {input: "$text", regex: /म/}, [{"match": "म", "idx": 0, "captures": []}]);
    // Unicode with capture group.
    testRegexFindAggForKey(3, {input: "$text", regex: /(गो )|(missing)/}, [
        {"match": "गो ", "idx": 3, "captures": ["गो ", null]},
    ]);
    // Test that regexes support Unicode character properties.
    testRegexFindAggForKey(2, {input: "$text", regex: String.raw`\p{Hangul}`}, []);
    testRegexFindAggForKey(2, {input: "$text", regex: String.raw`\p{Latin}+$`}, [
        {"match": "cafétéria", "idx": 0, "captures": []},
    ]);
    // Test that the (*UTF) and (*UTF8) options are accepted for unicode characters.
    assert.commandWorked(coll.insert({_id: 12, text: "༢༣༤༤༤༥12༥A"}));
    testRegexFindAggForKey(12, {input: "$text", regex: "(*UTF8)༤"}, [
        {"match": "༤", "idx": 2, "captures": []},
        {"match": "༤", "idx": 3, "captures": []},
        {"match": "༤", "idx": 4, "captures": []},
    ]);
    testRegexFindAggForKey(12, {input: "$text", regex: "(*UTF)༤"}, [
        {"match": "༤", "idx": 2, "captures": []},
        {"match": "༤", "idx": 3, "captures": []},
        {"match": "༤", "idx": 4, "captures": []},
    ]);
    // For ASCII characters.
    assert.commandWorked(coll.insert({_id: 4, text: "123444"}));
    testRegexFindAggForKey(4, {input: "$text", regex: "(*UTF8)(44)"}, [{"match": "44", "idx": 3, "captures": ["44"]}]);
    testRegexFindAggForKey(4, {input: "$text", regex: "(*UTF)(44)"}, [{"match": "44", "idx": 3, "captures": ["44"]}]);

    // When the (*UCP) option is specified, Unicode "word" characters are included in the '\w'
    // character type.
    testRegexFindAggForKey(12, {input: "$text", regex: String.raw`(*UCP)^(\w+)`}, [
        {"match": "༢༣༤༤༤༥12༥A", "idx": 0, "captures": ["༢༣༤༤༤༥12༥A"]},
    ]);
    // When the (*UCP) option is specified, [:digit:] becomes \p{N} and matches all Unicode
    // decimal digit characters.
    testRegexFindAggForKey(12, {input: "$text", regex: "(*UCP)^[[:digit:]]+"}, [
        {"match": "༢༣༤༤༤༥12༥", "idx": 0, "captures": []},
    ]);
    testRegexFindAggForKey(12, {input: "$text", regex: "(*UCP)[[:digit:]]+$"}, []);
    // When the (*UCP) option is specified, [:alpha:] becomes \p{L} and matches all Unicode
    // alphabetic characters.
    assert.commandWorked(coll.insert({_id: 13, text: "박정수AB"}));
    testRegexFindAggForKey(13, {input: "$text", regex: String.raw`(*UCP)^[[:alpha:]]+`}, [
        {"match": "박정수AB", "idx": 0, "captures": []},
    ]);

    // No match when options are not set.
    testRegexFindAggForKey(12, {input: "$text", regex: String.raw`^(\w+)`}, []);
    testRegexFindAggForKey(12, {input: "$text", regex: "^[[:digit:]]"}, []);
    testRegexFindAggForKey(2, {input: "$text", regex: "^[[:alpha:]]+$"}, []);

    // The 'u' option shouldn't throw an error.
    testRegexFindAggForKey(2, {input: "$text", regex: "^[[:alpha:]]+$", options: "u"}, []);
})();

(function testErrors() {
    coll.drop();
    assert.commandWorked(coll.insert({text: "string"}));
    // Null object.
    testRegexAggException(null, 51103);
    // Incorrect object parameter.
    testRegexAggException("incorrect type", 51103);
    // Test malformed regex.
    testRegexAggException({input: "$text", regex: "[0-9"}, 51111);
    testRegexAggException({regex: "[a-c", input: null}, 51111);
    // Malformed regex because start options not at the beginning.
    testRegexAggException({input: "$text", regex: "^(*UCP)[[:alpha:]]+$"}, 51111);
    testRegexAggException({input: "$text", regex: "((*UCP)[[:alpha:]]+$)"}, 51111);
    // At least one of the 'input' field is not string.
    assert.commandWorked(coll.insert({a: "string"}));
    assert.commandWorked(coll.insert({a: {b: "object"}}));
    testRegexAggException({input: "$a", regex: "valid"}, 51104);
    testRegexAggException({input: "$a", regex: null}, 51104);
    // 'regex' field is not string or regex.
    testRegexAggException({input: "$text", regex: ["incorrect"]}, 51105);
    // 'options' field is not string.
    testRegexAggException({input: "$text", regex: "valid", options: 123}, 51106);
    // Incorrect 'options' flag.
    testRegexAggException({input: "$text", regex: "valid", options: "a"}, 51108);
    // 'options' are case-sensitive.
    testRegexAggException({input: "$text", regex: "valid", options: "I"}, 51108);
    // Options specified in both 'regex' and 'options'.
    testRegexAggException({input: "$text", regex: /(m(p))/i, options: "i"}, 51107);
    testRegexAggException({input: "$text", regex: /(m(p))/i, options: "x"}, 51107);
    testRegexAggException({input: "$text", regex: /(m(p))/m, options: ""}, 51107);
    // 'regex' as string with null characters.
    testRegexAggException({input: "$text", regex: "sasd\0", options: "i"}, 51109);
    testRegexAggException({regex: "sa\x00sd", options: "i", input: null}, 51109);
    // 'options' as string with null characters.
    testRegexAggException({input: "$text", regex: /(m(p))/, options: "i\0"}, 51110);
    testRegexAggException({input: "$text", options: "i\x00", regex: null}, 51110);
    // Invalid parameter.
    testRegexAggException({input: "$text", invalid: "i"}, 31024);
    testRegexAggException({input: "$text", regex: "sa", invalid: "$missingField"}, 31024);
    testRegexAggException({input: "$text", regex: "sa", invalid: null}, 31024);
    testRegexAggException({input: "$text", regex: "sa", invalid: []}, 31024);
    // Regex not present.
    testRegexAggException({input: "$text"}, 31023);
    testRegexAggException({input: "$missingField"}, 31023);
    testRegexAggException({input: "$text", options: "invalid"}, 31023);
    // Input not present.
    testRegexAggException({regex: /valid/}, 31022);
    testRegexAggException({regex: "$missingField"}, 31022);
    testRegexAggException({regex: "[0-9"}, 31022);
    // Empty object.
    testRegexAggException({}, 31022);
})();

(function testMultipleMatches() {
    coll.drop();
    assert.commandWorked(coll.insert({a: "string1string2", regex: "(string[1-2])"}));
    assert.commandWorked(coll.insert({a: "string3 string4", regex: "(string[3-4])"}));
    assert.commandWorked(coll.insert({a: "string5 string6", regex: "(string[3-4])"}));
    // All documents match.
    testRegexFindAgg({input: "$a", regex: "(str.*?[0-9])"}, [
        {
            "matches": [
                {"match": "string1", "idx": 0, "captures": ["string1"]},
                {"match": "string2", "idx": 7, "captures": ["string2"]},
            ],
        },
        {
            "matches": [
                {"match": "string3", "idx": 0, "captures": ["string3"]},
                {"match": "string4", "idx": 8, "captures": ["string4"]},
            ],
        },
        {
            "matches": [
                {"match": "string5", "idx": 0, "captures": ["string5"]},
                {"match": "string6", "idx": 8, "captures": ["string6"]},
            ],
        },
    ]);
    // Only one match.
    testRegexFindAgg({input: "$a", regex: "(^.*[0-2]$)"}, [
        {"matches": []},
        {"matches": []},
        {"matches": [{"match": "string1string2", "idx": 0, "captures": ["string1string2"]}]},
    ]);
    // None match.
    testRegexFindAgg({input: "$a", regex: "(^.*[7-9]$)"}, [{"matches": []}, {"matches": []}, {"matches": []}]);

    // All documents match when using variable regex.
    testRegexFindAgg({input: "$a", regex: "$regex"}, [
        {"matches": []},
        {
            "matches": [
                {"match": "string1", "idx": 0, "captures": ["string1"]},
                {"match": "string2", "idx": 7, "captures": ["string2"]},
            ],
        },
        {
            "matches": [
                {"match": "string3", "idx": 0, "captures": ["string3"]},
                {"match": "string4", "idx": 8, "captures": ["string4"]},
            ],
        },
    ]);
})();

(function testInsideCondOperator() {
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, level: "Public Knowledge", info: "Company Name"}));
    assert.commandWorked(coll.insert({_id: 1, level: "Private Information", info: "Company Secret"}));
    const expectedResults = [
        {"_id": 0, "information": "Company Name"},
        {"_id": 1, "information": "REDACTED"},
    ];
    // For $regexFindAll.
    const resultFindAll = coll
        .aggregate([
            {
                "$project": {
                    "information": {
                        "$cond": [
                            {"$eq": [{"$regexFindAll": {input: "$level", regex: /public/i}}, []]},
                            "REDACTED",
                            "$info",
                        ],
                    },
                },
            },
        ])
        .toArray();
    assert(anyEq(resultFindAll, expectedResults));
    // For $regexMatch.
    const resultMatch = coll
        .aggregate([
            {
                "$project": {
                    "information": {
                        "$cond": [{"$regexMatch": {input: "$level", regex: /public/i}}, "$info", "REDACTED"],
                    },
                },
            },
        ])
        .toArray();
    // For $regexFind.
    const resultFind = coll
        .aggregate([
            {
                "$project": {
                    "information": {
                        "$cond": [
                            {"$ne": [{"$regexFind": {input: "$level", regex: /public/i}}, null]},
                            "$info",
                            "REDACTED",
                        ],
                    },
                },
            },
        ])
        .toArray();
    // Validate that {$ne : [{$regexFind: ...}, null]} produces same result as
    // {$regexMatch: ...}.
    assert(anyEq(resultFind, resultMatch));
    assert(anyEq(resultFind, expectedResults));
})();
