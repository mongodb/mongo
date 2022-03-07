// SERVER-62242
// $indexOfArray does not work with duplicate values in array
(function() {
"use strict";

const c = db[jsTest.name()];
c.drop();

c.save({_id: 0, number: 222});

// before SERVER-62242, this incorrectly returned {_id: 0, number: 222, idx: -1}
assert.eq(
    [{_id: 0, number: 222, idx: 2}],
    c.aggregate([{$addFields: {idx: {$indexOfArray: [[111, 111, 222], "$number"]}}}]).toArray());

// this query was OK
assert.eq([{_id: 0, number: 222, idx: 2}],
          c.aggregate([
               {$addFields: {idx: {$indexOfArray: [[111, 111, 222, 333], "$number"]}}}
           ]).toArray());

// also test for cases where a range is specified
assert.eq([{_id: 0, number: 222, idx: -1}],
          c.aggregate([
               {$addFields: {idx: {$indexOfArray: [[111, 111, 222, 333], "$number", 0, 1]}}}
           ]).toArray());

assert.eq([{_id: 0, number: 222, idx: 3}],
          c.aggregate([
               {$addFields: {idx: {$indexOfArray: [[111, 111, 222, 222, 333], "$number", 3, 5]}}}
           ]).toArray());
})();
