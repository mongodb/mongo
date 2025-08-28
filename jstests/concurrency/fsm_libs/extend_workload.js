import {parseConfig} from "jstests/concurrency/fsm_libs/parse_config.js";

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
export function extendWorkload($config, callback) {
    assert.eq(2, arguments.length, "extendWorkload must be called with 2 arguments: $config and callback");
    assert.eq("function", typeof callback, "2nd argument to extendWorkload must be a callback");
    assert.eq(2, callback.length, "2nd argument to extendWorkload must take 2 arguments: $config and $super");
    let parsedSuperConfig = parseConfig($config);
    let childConfig = Object.extend({}, parsedSuperConfig, true);
    return callback(childConfig, parsedSuperConfig);
}
