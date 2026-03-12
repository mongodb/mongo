// This file is adapted from https://www.npmjs.com/package/fast-check

// Shim for resmoke.py compatibility

/* global setTimeout, clearTimeout, Reflect*/

// Provide local shims if the host does not define them.

/// ####
/// Shim
/// ####

var setTimeout =
    typeof setTimeout === "function"
        ? setTimeout
        : function (fn, ms) {
              // Mongo shell has sleep(ms); if present, use it.
              if (typeof sleep === "function") {
                  sleep(ms);
                  fn();
                  return 0;
              }
              // Fallback: run immediately
              fn();
              return 0;
          };

var clearTimeout =
    typeof clearTimeout === "function"
        ? clearTimeout
        : function (_id) {
              /* no-op */
          };

// --- Reflect.ownKeys shim ---
var Reflect = typeof Reflect !== "undefined" ? Reflect : {};

if (typeof Reflect.ownKeys !== "function") {
    Reflect.ownKeys = function (target) {
        var keys = Object.getOwnPropertyNames(target);
        if (typeof Object.getOwnPropertySymbols === "function") {
            keys = keys.concat(Object.getOwnPropertySymbols(target));
        }
        return keys;
    };
}

/// #############################
/// Bundled version of fast-check
/// #############################
