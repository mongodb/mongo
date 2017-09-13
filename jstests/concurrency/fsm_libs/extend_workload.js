'use strict';

load('jstests/concurrency/fsm_libs/parse_config.js');  // for parseConfig

/** extendWorkload usage:
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
    var parsedSuperConfig = parseConfig($config);
    var childConfig = Object.extend({}, parsedSuperConfig, true);
    return callback(childConfig, parsedSuperConfig);
}
