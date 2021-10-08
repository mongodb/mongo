/**
 * Validates that the planCacheSize parameter is set correctly at startup.
 */

(function() {
"use strict";

const paramName = "planCacheSize";
const paramValue = "711MB";

let mdb = MongoRunner.runMongod({setParameter: `${paramName}=${paramValue}`});
let result = mdb.getDB("admin").runCommand({getParameter: 1, [paramName]: 1});
assert.eq(result[paramName], paramValue);

MongoRunner.stopMongod(mdb);
})();
