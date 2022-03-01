<!--
Copyright (C) 2016 and later: Unicode, Inc. and others.
License & terms of use: http://www.unicode.org/copyright.html

Copyright (c) 2015-2016, International Business Machines Corporation and others. All Rights Reserved.
-->

This directory contains the break iterator reference rule files used by intltest rbbi/RBBIMonkeyTest/testMonkey.
===========================================

The rules in this directory track the boundary rules from Unicode UAX 14 and 29. They are interpreted
to provide an expected set of boundary positions to compare with the results from ICU break iteration.

ICU4J also includes copies of the test reference rules, located in the directory
main/tests/core/src/com/ibm/icu/dev/test/rbbi/break_rules/
The copies should be kept synchronized; there should be no differences.

Each set of reference break rules lives in a separate file.
The list of rule files to run by default is hard coded into the test code, in rbbimonkeytest.cpp.

Each test file includes
  - The type of ICU break iterator to create (word, line, sentence, etc.)
  - The locale to use
  - Character Class definitions
  - Rule definitions

To Do
  - Extend the syntax to support rule tailoring.


**character class definition**

        name = set_regular_expression;

*caution* When referenced, these definitions are textually substituted into the overall rule.
To avoid unexpected behavior, include [brackets] around the full definition

        letter_number = [:Letter:][:Number:];

Will compile, but will produce unexpected results.

        letter_number = [[:Letter:][:Number:]];

is safe. The issue is similar to the problems that can occur with the C preprocessor
and the use of parentheses around macro paramteters.

**rule definition**

        rule_regular_expression;

**name**

        [A-Za-z_][A-Za-z0-9_]*

**set_regular_expression**

The intersection of an ICU regular expression [set] expression and a UnicodeSet pattern
(They are mostly the same). May include previously defined set names, which are logically
expanded in-place.

**rule_regular_expression**

        An ICU Regular Expression.
        May include set names, which are logically expanded in-place.
        May include a '÷', which defines a boundary position.

Application of the rules:

Matching begins at the start of text, or after a previously identified boundary.
The pseudo-code below finds the next boundary.

    while position < end of text
        for each rule
            if the text at position matches this rule
                if the rule has a '÷'
                    Boundary is found.
                    return the position of the '÷' within the match.
                else
                    position = last character of the rule match.
                    break from the inner rule loop, continue the outer loop.

This differs from the Unicode UAX algorithm in that each position in the text is
not tested separately. Instead, when a rule match is found, rule application restarts with the last
character of the preceding rule match. ICU's break rules also operate this way.

Expressing rules this way simplifies UAX rules that have leading or trailing context; it
is no longer necessary to write expressions that match the context starting from
any position within it.

This rule form differs from ICU rules in that the rules are applied sequentially, as they
are with the Unicode UAX rules. With the main ICU break rules, all are applied in parallel.

**Word Dictionaries**


The monkey test does not test dictionary based breaking. The set named 'dictionary' is special,
as it is in the main ICU rules. For the monkey test, no characters from the dictionary set are
included in the randomly-generated test data.


