/**
 * Tests the shell util '_compareStringsWithCollation'
 */

assert.eq(_compareStringsWithCollation("abc", "abc", {locale: "en_US"}), 0);
assert.gt(_compareStringsWithCollation("bcd", "abc", {locale: "en_US"}), 0);
assert.lt(_compareStringsWithCollation("abc", "ABC", {locale: "en_US"}), 0);

// zero length strings and null bytes
assert.eq(_compareStringsWithCollation("", "", {locale: "en_US"}), 0);
assert.gt(_compareStringsWithCollation("abc", "", {locale: "en_US"}), 0);
assert.gt(_compareStringsWithCollation("abc", "", {locale: "en_US", strength: 2}), 0);
assert.eq(_compareStringsWithCollation("\0", "", {locale: "en_US"}), 0);
assert.lt(_compareStringsWithCollation("\0", "ab", {locale: "en_US"}), 0);
assert.gt(_compareStringsWithCollation("a\0c", "a\0b", {locale: "en_US"}), 0);
assert.eq(_compareStringsWithCollation("a", "a\0", {locale: "en_US"}), 0);

// case-level and diatrics
assert.eq(_compareStringsWithCollation("abc", "ABC", {locale: "en_US", strength: 1}), 0);
assert.eq(_compareStringsWithCollation("abc", "ABC", {locale: "en_US", strength: 2}), 0);
assert.lt(_compareStringsWithCollation("abc", "ABC", {locale: "en_US", strength: 3}), 0);
assert.lt(
    _compareStringsWithCollation("abc", "ABC", {locale: "en_US", strength: 1, caseLevel: true}), 0);
assert.lt(
    _compareStringsWithCollation("abc", "ABC", {locale: "en_US", strength: 2, caseLevel: true}), 0);

assert.eq(_compareStringsWithCollation("eaio", "éáïô", {locale: "en_US", strength: 1}), 0);
assert.lt(_compareStringsWithCollation("eaio", "éáïô", {locale: "en_US", strength: 2}), 0);

assert.gt(_compareStringsWithCollation("abc", "ABC", {locale: "en_US", caseFirst: "upper"}), 0);
assert.lt(_compareStringsWithCollation("abc", "ABC", {locale: "en_US", caseFirst: "lower"}), 0);

// numeric ordering
assert.gt(_compareStringsWithCollation("10", "2", {locale: "en_US", numericOrdering: true}), 0);
assert.lt(_compareStringsWithCollation("10", "2", {locale: "en_US", numericOrdering: false}), 0);

// Ignore whitespace and punctuation
assert.eq(_compareStringsWithCollation("a b, c", "abc", {locale: "en_US", alternate: "shifted"}),
          0);
assert.neq(_compareStringsWithCollation(
               "a b, c", "abc", {locale: "en_US", strength: 4, alternate: "shifted"}),
           0);
assert.eq(_compareStringsWithCollation(
              "a b, c", "abc", {locale: "en_US", alternate: "shifted", maxVariable: "punct"}),
          0);
assert.neq(_compareStringsWithCollation(
               "a b, c", "abc", {locale: "en_US", alternate: "shifted", maxVariable: "space"}),
           0);
assert.eq(_compareStringsWithCollation(
              "a b c", "abc", {locale: "en_US", alternate: "shifted", maxVariable: "space"}),
          0);

// error cases
assert.throwsWithCode(() => _compareStringsWithCollation("", ""), 9367804);
assert.throwsWithCode(() => _compareStringsWithCollation(1, "", {locale: "en_US"}), 9367801);
assert.throwsWithCode(() => _compareStringsWithCollation("", 1, {locale: "en_US"}), 9367803);
assert.throwsWithCode(() => _compareStringsWithCollation({a: ""}, "", {locale: "en_US"}), 9367801);
assert.throwsWithCode(() => _compareStringsWithCollation("", "", ""), 9367805);
assert.throwsWithCode(() => _compareStringsWithCollation("", "", {}), 40414);
