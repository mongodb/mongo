'use strict';

load('jstests/concurrency/fsm_libs/parse_config.js');  // for parseConfig

/**
 * extendWorkload usage:
 *
 * $config = extendWorkload($config, function($config, $super) {
 *   // ... modify $config ...
 *   $config.foo = function() { // override a method
 *     $super.foo.call(this, arguments); // call super
 *   };
 *   return $config;
 * });
 */
function extendWorkload($config, callback) {
    assert.eq(2,
              arguments.length,
              'extendWorkload must be called with 2 arguments: $config and callback');
    assert.eq('function', typeof callback, '2nd argument to extendWorkload must be a callback');
    assert.eq(2,
              callback.length,
              '2nd argument to extendWorkload must take 2 arguments: $config and $super');
    let parsedSuperConfig = parseConfig($config);
    let childConfig = Object.extend({}, parsedSuperConfig, true);
    return callback(childConfig, parsedSuperConfig);
}

/**
 * assignEqualProbsToTransitionsFromTotal example usage:
 * $config.transitions = Object.extend({<state>:
 * assignEqualProbsToTransitionsFromTotal(Object.keys($config.states))}, $super.transitions, 1.0);
 * `totalProbability` refers to the total probability that is divided amongst the members of
 * `states`. For example, if `totalProbability` is 0.8 and there are two states, then each gets 0.4
 * probability.
 */
function assignEqualProbsToTransitionsFromTotal(states, totalProbability) {
    assertAlways.gt(states.length, 0);
    let probs = {};
    let pr = totalProbability / states.length;
    states.forEach(function(s) {
        probs[s] = pr;
    });
    return probs;
}

/**
 * Like assignEqualProbsToTransitionsFromTotal, but uses 1.0 as the total probability.
 */
function assignEqualProbsToTransitions(states) {
    return assignEqualProbsToTransitionsFromTotal(states, 1.0);
}
