/**
 * SERVER-14670 introduced the $strLenBytes and $strLenCP aggregation expressions. In this file, we
 * test their expected behaviour.
 * */

import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

var coll = db.substr;
assert(coll.drop());

assert.commandWorked(coll.insert({
    strField: "MyString",
    intField: 1,
    nullField: null,
    specialCharField: "é",
    specialCharField2: "匣6卜十戈大中金",
    specialCharField3: "i ♥ u"
}));

// $strLenCP
assertErrorCode(
    coll, [{$project: {strLen: {$strLenCP: 1}}}], 5155900, "$strLenCP requires a string argument.");

assertErrorCode(coll,
                [{$project: {strLen: {$strLenCP: "$intField"}}}],
                5155900,
                "$strLenCP requires a string argument.");

assertErrorCode(coll,
                [{$project: {strLen: {$strLenCP: "$nullField"}}}],
                5155900,
                "$strLenCP requires a string argument.");

assertErrorCode(coll,
                [{$project: {strLen: {$strLenCP: "$b"}}}],
                5155900,
                "$strLenCP requires a string argument.");

assert.commandFailed(
    coll.runCommand('aggregate', {pipeline: [{$project: {a: {$strLenCP: null}}}]}));

assertErrorCode(coll,
                [{$project: {strLen: {$strLenCP: ["hello", "hello"]}}}],
                16020,
                "$strLenCP requires a one argument.");
// $strLenBytes
assertErrorCode(coll,
                [{$project: {strLen: {$strLenBytes: "$intField"}}}],
                5155800,
                "$strLenBytes requires a string argument");

assertErrorCode(coll,
                [{$project: {strLen: {$strLenBytes: "$nullField"}}}],
                5155800,
                "$strLenBytes requires a string argument");

assertErrorCode(coll,
                [{$project: {strLen: {$strLenBytes: "$b"}}}],
                5155800,
                "$strLenBytes requires a string argument");

assert.commandFailed(
    coll.runCommand('aggregate', {pipeline: [{$project: {a: {$strLenBytes: null}}}]}));

assertErrorCode(coll,
                [{$project: {strLen: {$strLenBytes: ["hello", "hello"]}}}],
                16020,
                "$strLenBytes requires one parameter");

// Checks that strLenBytes and strLenCP return different things for multi-byte characters.
assert.eq({"strLenBytes": 2, "strLenCP": 1},
          coll.aggregate({
                  $project: {
                      _id: 0,
                      strLenBytes: {$strLenBytes: "$specialCharField"},
                      strLenCP: {$strLenCP: "$specialCharField"}
                  }
              })
              .toArray()[0]);

assert.eq({"strLenBytes": 22, "strLenCP": 8},
          coll.aggregate({
                  $project: {
                      _id: 0,
                      strLenBytes: {$strLenBytes: "$specialCharField2"},
                      strLenCP: {$strLenCP: "$specialCharField2"}
                  }
              })
              .toArray()[0]);

assert.eq({"strLenBytes": 7, "strLenCP": 5},
          coll.aggregate({
                  $project: {
                      _id: 0,
                      strLenBytes: {$strLenBytes: "i ♥ u"},
                      strLenCP: {$strLenCP: "$specialCharField3"}
                  }
              })
              .toArray()[0]);

assert.eq({"strLenBytes": 20, "strLenCP": 5},
          coll.aggregate({
                  $project: {
                      _id: 0,
                      strLenBytes: {$strLenBytes: "🧐🤓😎🥸🤩"},
                      strLenCP: {$strLenCP: "🧐🤓😎🥸🤩"}
                  }
              })
              .toArray()[0]);

assert.eq({"strLenBytes": 24, "strLenCP": 9},
          coll.aggregate({
                  $project: {
                      _id: 0,
                      strLenBytes: {$strLenBytes: "十🤳🕺十十💃ahk"},
                      strLenCP: {$strLenCP: "十🤳🕺十十💃ahk"}
                  }
              })
              .toArray()[0]);

// Checks that strLenBytes and strLenCP returns same thing for single byte characters
assert.eq({"strLenBytes": 8, "strLenCP": 8},
          coll.aggregate({
                  $project: {
                      _id: 0,
                      strLenBytes: {$strLenBytes: "$strField"},
                      strLenCP: {$strLenCP: "$strField"}
                  }
              })
              .toArray()[0]);
