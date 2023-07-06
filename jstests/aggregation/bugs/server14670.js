/**
 * SERVER-14670 introduced the $strLenBytes and $strLenCP aggregation expressions. In this file, we
 * test their expected behaviour.
 * */

import "jstests/libs/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

var coll = db.substr;
assert(coll.drop());

assert.commandWorked(
    coll.insert({strField: "MyString", intField: 1, nullField: null, specialCharField: "é"}));

assertErrorCode(
    coll, [{$project: {strLen: {$strLenCP: 1}}}], 34471, "$strLenCP requires a string argument.");

assert.eq({"strLen": 8},
          coll.aggregate({$project: {_id: 0, strLen: {$strLenBytes: "$strField"}}}).toArray()[0]);

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
