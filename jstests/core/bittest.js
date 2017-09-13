/**
 * This test ensures that bit test query operators work.
 */
(function() {
    'use strict';

    load("jstests/libs/analyze_plan.js");

    var coll = db.jstests_bitwise;

    function assertQueryCorrect(query, count) {
        var explain = coll.find(query).explain("executionStats");
        assert(isCollscan(explain.queryPlanner.winningPlan),
               "expected bit test query plan to be COLLSCAN");
        assert.eq(count,
                  explain.executionStats.nReturned,
                  "bit test query not returning correct documents");
    }

    // Tests on numbers.

    coll.drop();
    assert.writeOK(coll.insert({a: 0}));
    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({a: 54}));
    assert.writeOK(coll.insert({a: 88}));
    assert.writeOK(coll.insert({a: 255}));
    assert.commandWorked(coll.createIndex({a: 1}));

    // Tests with bitmask.
    assertQueryCorrect({a: {$bitsAllSet: 0}}, 5);
    assertQueryCorrect({a: {$bitsAllSet: 1}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: 16}}, 3);
    assertQueryCorrect({a: {$bitsAllSet: 54}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: 55}}, 1);
    assertQueryCorrect({a: {$bitsAllSet: 88}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: 255}}, 1);
    assertQueryCorrect({a: {$bitsAllClear: 0}}, 5);
    assertQueryCorrect({a: {$bitsAllClear: 1}}, 3);
    assertQueryCorrect({a: {$bitsAllClear: 16}}, 2);
    assertQueryCorrect({a: {$bitsAllClear: 129}}, 3);
    assertQueryCorrect({a: {$bitsAllClear: 255}}, 1);
    assertQueryCorrect({a: {$bitsAnySet: 0}}, 0);
    assertQueryCorrect({a: {$bitsAnySet: 9}}, 3);
    assertQueryCorrect({a: {$bitsAnySet: 255}}, 4);
    assertQueryCorrect({a: {$bitsAnyClear: 0}}, 0);
    assertQueryCorrect({a: {$bitsAnyClear: 18}}, 3);
    assertQueryCorrect({a: {$bitsAnyClear: 24}}, 3);
    assertQueryCorrect({a: {$bitsAnyClear: 255}}, 4);

    // Tests with array of bit positions.
    assertQueryCorrect({a: {$bitsAllSet: []}}, 5);
    assertQueryCorrect({a: {$bitsAllSet: [0]}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: [4]}}, 3);
    assertQueryCorrect({a: {$bitsAllSet: [1, 2, 4, 5]}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: [0, 1, 2, 4, 5]}}, 1);
    assertQueryCorrect({a: {$bitsAllSet: [3, 4, 6]}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: [0, 1, 2, 3, 4, 5, 6, 7]}}, 1);
    assertQueryCorrect({a: {$bitsAllClear: []}}, 5);
    assertQueryCorrect({a: {$bitsAllClear: [0]}}, 3);
    assertQueryCorrect({a: {$bitsAllClear: [4]}}, 2);
    assertQueryCorrect({a: {$bitsAllClear: [1, 7]}}, 3);
    assertQueryCorrect({a: {$bitsAllClear: [0, 1, 2, 3, 4, 5, 6, 7]}}, 1);
    assertQueryCorrect({a: {$bitsAnySet: []}}, 0);
    assertQueryCorrect({a: {$bitsAnySet: [1, 3]}}, 3);
    assertQueryCorrect({a: {$bitsAnySet: [0, 1, 2, 3, 4, 5, 6, 7]}}, 4);
    assertQueryCorrect({a: {$bitsAnyClear: []}}, 0);
    assertQueryCorrect({a: {$bitsAnyClear: [1, 4]}}, 3);
    assertQueryCorrect({a: {$bitsAnyClear: [3, 4]}}, 3);
    assertQueryCorrect({a: {$bitsAnyClear: [0, 1, 2, 3, 4, 5, 6, 7]}}, 4);

    // Tests with multiple predicates.
    assertQueryCorrect({a: {$bitsAllSet: 54, $bitsAllClear: 201}}, 1);

    // Tests on negative numbers.

    coll.drop();
    assert.writeOK(coll.insert({a: -0}));
    assert.writeOK(coll.insert({a: -1}));
    assert.writeOK(coll.insert({a: -54}));

    // Tests with bitmask.
    assertQueryCorrect({a: {$bitsAllSet: 0}}, 3);
    assertQueryCorrect({a: {$bitsAllSet: 2}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: 127}}, 1);
    assertQueryCorrect({a: {$bitsAllSet: 74}}, 2);
    assertQueryCorrect({a: {$bitsAllClear: 0}}, 3);
    assertQueryCorrect({a: {$bitsAllClear: 53}}, 2);
    assertQueryCorrect({a: {$bitsAllClear: 127}}, 1);
    assertQueryCorrect({a: {$bitsAnySet: 0}}, 0);
    assertQueryCorrect({a: {$bitsAnySet: 2}}, 2);
    assertQueryCorrect({a: {$bitsAnySet: 127}}, 2);
    assertQueryCorrect({a: {$bitsAnyClear: 0}}, 0);
    assertQueryCorrect({a: {$bitsAnyClear: 53}}, 2);
    assertQueryCorrect({a: {$bitsAnyClear: 127}}, 2);

    // Tests with array of bit positions.
    var allPositions = [];
    for (var i = 0; i < 64; i++) {
        allPositions.push(i);
    }
    assertQueryCorrect({a: {$bitsAllSet: []}}, 3);
    assertQueryCorrect({a: {$bitsAllSet: [1]}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: allPositions}}, 1);
    assertQueryCorrect({a: {$bitsAllSet: [1, 7, 6, 3, 100]}}, 2);
    assertQueryCorrect({a: {$bitsAllClear: []}}, 3);
    assertQueryCorrect({a: {$bitsAllClear: [5, 4, 2, 0]}}, 2);
    assertQueryCorrect({a: {$bitsAllClear: allPositions}}, 1);
    assertQueryCorrect({a: {$bitsAnySet: []}}, 0);
    assertQueryCorrect({a: {$bitsAnySet: [1]}}, 2);
    assertQueryCorrect({a: {$bitsAnySet: allPositions}}, 2);
    assertQueryCorrect({a: {$bitsAnyClear: []}}, 0);
    assertQueryCorrect({a: {$bitsAnyClear: [0, 2, 4, 5, 100]}}, 2);
    assertQueryCorrect({a: {$bitsAnyClear: allPositions}}, 2);

    // Tests with multiple predicates.
    assertQueryCorrect({a: {$bitsAllSet: 74, $bitsAllClear: 53}}, 1);

    // Tests on BinData.

    coll.drop();
    assert.writeOK(coll.insert({a: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")}));
    assert.writeOK(coll.insert({a: BinData(0, "AANgAAAAAAAAAAAAAAAAAAAAAAAA")}));
    assert.writeOK(coll.insert({a: BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")}));
    assert.writeOK(coll.insert({a: BinData(0, "////////////////////////////")}));
    assert.commandWorked(coll.createIndex({a: 1}));

    // Tests with binary string bitmask.
    assertQueryCorrect({a: {$bitsAllSet: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")}}, 4);
    assertQueryCorrect({a: {$bitsAllSet: BinData(0, "AANgAAAAAAAAAAAAAAAAAAAAAAAA")}}, 3);
    assertQueryCorrect({a: {$bitsAllSet: BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")}}, 2);
    assertQueryCorrect({a: {$bitsAllSet: BinData(0, "////////////////////////////")}}, 1);
    assertQueryCorrect({a: {$bitsAllClear: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")}}, 4);
    assertQueryCorrect({a: {$bitsAllClear: BinData(0, "AAyfAAAAAAAAAAAAAAAAAAAAAAAA")}}, 3);
    assertQueryCorrect({a: {$bitsAllClear: BinData(0, "JAyfqwetkqwklEWRbWERKKJREtbq")}}, 2);
    assertQueryCorrect({a: {$bitsAllClear: BinData(0, "////////////////////////////")}}, 1);
    assertQueryCorrect({a: {$bitsAnySet: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")}}, 0);
    assertQueryCorrect({a: {$bitsAnySet: BinData(0, "AAyfAAAAAAAAAAAAAAAAAAAAAAAA")}}, 1);
    assertQueryCorrect({a: {$bitsAnySet: BinData(0, "JAyfqwetkqwklEWRbWERKKJREtbq")}}, 2);
    assertQueryCorrect({a: {$bitsAnySet: BinData(0, "////////////////////////////")}}, 3);
    assertQueryCorrect({a: {$bitsAnyClear: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")}}, 0);
    assertQueryCorrect({a: {$bitsAnyClear: BinData(0, "AANgAAAAAAAAAAAAAAAAAAAAAAAA")}}, 1);
    assertQueryCorrect({a: {$bitsAnyClear: BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")}}, 2);
    assertQueryCorrect({a: {$bitsAnyClear: BinData(0, "////////////////////////////")}}, 3);

    // Tests with multiple predicates.
    assertQueryCorrect({
        a: {
            $bitsAllSet: BinData(0, "AANgAAAAAAAAAAAAAAAAAAAAAAAA"),
            $bitsAllClear: BinData(0, "//yf////////////////////////")
        }
    },
                       1);

    coll.drop();
})();