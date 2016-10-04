'use strict';

/*
 * This file tests the FSM test framework.
 */

load('jstests/concurrency/fsm_libs/fsm.js');

(function() {
    var getWeightedRandomChoice = fsm._getWeightedRandomChoice;

    var doc = {a: 0.25, b: 0.5, c: 0.25};

    // NOTE: getWeightedRandomChoice calls assert internally, so it will print stack traces
    // when assert.throws executes
    assert.throws(function() {
        getWeightedRandomChoice(doc, -1);
    }, [], 'should reject negative values');
    assert.throws(function() {
        getWeightedRandomChoice(doc, 1);
    }, [], 'should reject values == 1');
    assert.throws(function() {
        getWeightedRandomChoice(doc, 2);
    }, [], 'should reject values > 1');

    assert.throws(function() {
        getWeightedRandomChoice({}, 0.0);
    }, [], 'cannot choose from zero states');
    assert.throws(function() {
        getWeightedRandomChoice({}, 0.5);
    }, [], 'cannot choose from zero states');
    assert.throws(function() {
        getWeightedRandomChoice({}, 0.99);
    }, [], 'cannot choose from zero states');

    assert.eq('a', getWeightedRandomChoice(doc, 0.00), '0');
    assert.eq('a', getWeightedRandomChoice(doc, 0.24), '1');
    assert.eq('b', getWeightedRandomChoice(doc, 0.25), '2');
    assert.eq('b', getWeightedRandomChoice(doc, 0.50), '3');
    assert.eq('b', getWeightedRandomChoice(doc, 0.74), '4');
    assert.eq('c', getWeightedRandomChoice(doc, 0.75), '5');
    assert.eq('c', getWeightedRandomChoice(doc, 0.99), '6');
})();
