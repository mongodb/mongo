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
 * assignEqualProbsToTransitions example usage:
 * $config.transitions = Object.extend({<state>:
 * assignEqualProbsToTransitions(Object.keys($config.states))}, $super.transitions);
 */
function assignEqualProbsToTransitions(states) {
    assertAlways.gt(states.length, 0);
    let probs = {};
    let pr = 1.0 / states.length;
    states.forEach(function(s) {
        probs[s] = pr;
    });
    return probs;
}
