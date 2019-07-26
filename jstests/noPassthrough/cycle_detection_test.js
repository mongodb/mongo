/**
 * Tests for the Graph#findCycle() method.
 */
(function() {
'use strict';

load('jstests/libs/cycle_detection.js');  // for Graph

(function testLinearChainHasNoCycle() {
    const graph = new Graph();
    graph.addEdge('A', 'B');
    graph.addEdge('B', 'C');
    graph.addEdge('C', 'D');

    assert.eq([], graph.findCycle());
})();

(function testGraphWithoutCycleButCommonAncestor() {
    const graph = new Graph();
    graph.addEdge('A', 'B');
    graph.addEdge('A', 'C');
    graph.addEdge('B', 'D');
    graph.addEdge('C', 'D');

    assert.eq([], graph.findCycle());
})();

(function testEmptyGraphHasNoCycle() {
    const graph = new Graph();
    assert.eq([], graph.findCycle());
})();

(function testGraphWithAllNodesInCycle() {
    const graph = new Graph();
    graph.addEdge(1, 2);
    graph.addEdge(2, 3);
    graph.addEdge(3, 4);
    graph.addEdge(4, 5);
    graph.addEdge(5, 1);

    assert.eq([1, 2, 3, 4, 5, 1], graph.findCycle());
})();

(function testGraphWithSomeNodesNotInCycle() {
    const graph = new Graph();
    graph.addEdge(1, 2);
    graph.addEdge(2, 3);
    graph.addEdge(3, 4);
    graph.addEdge(4, 5);
    graph.addEdge(5, 3);

    assert.eq([3, 4, 5, 3], graph.findCycle());
})();

(function testGraphWithSelfLoopConsideredCycle() {
    const graph = new Graph();
    graph.addEdge(0, 0);
    assert.eq([0, 0], graph.findCycle());
})();

(function testGraphUsesNonReferentialEquality() {
    const w = {a: new NumberInt(1)};
    const x = {a: new NumberInt(1)};
    const y = {a: new NumberLong(1)};
    const z = {a: 1};

    let graph = new Graph();
    graph.addEdge(w, x);
    assert.eq([w, x], graph.findCycle());

    graph = new Graph();
    graph.addEdge(w, y);
    assert.eq([], graph.findCycle());

    graph = new Graph();
    graph.addEdge(w, z);
    assert.eq([w, z], graph.findCycle());
})();

(function testGraphMinimizesCycleUsingNonReferentialEquality() {
    const graph = new Graph();
    graph.addEdge({a: 1}, {a: 2});
    graph.addEdge({a: 2}, {a: 3});
    graph.addEdge({a: 3}, {a: 4});
    graph.addEdge({a: 4}, {a: 5});
    graph.addEdge({a: 5}, {a: 3});

    assert.eq([{a: 3}, {a: 4}, {a: 5}, {a: 3}], graph.findCycle());
})();
})();
