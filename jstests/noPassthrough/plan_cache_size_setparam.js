/**
 * Validates that the planCacheSize parameter is set correctly at startup and runtime.
 */

(function() {
"use strict";

const paramName = "planCacheSize";
const paramDefaultValue = "5%";

let mongod = MongoRunner.runMongod();
let adminDb = mongod.getDB("admin");

function validatePlanCacheSize(paramValue) {
    let result = adminDb.runCommand({getParameter: 1, [paramName]: 1});
    assert.commandWorked(result);
    assert.eq(result[paramName], paramValue);
}

function setPlanCacheSizeThrows(paramValue, errorCode) {
    let result = adminDb.runCommand({setParameter: 1, [paramName]: paramValue});
    assert.commandFailedWithCode(result, errorCode);
}

// Validates that the parameter is set correctly at startup.
validatePlanCacheSize(paramDefaultValue);

// Validates that trying to set planCacheSize throws an error.
const paramCorrectValue = "100MB";
const errorCode = 7529500;
setPlanCacheSizeThrows(paramCorrectValue, errorCode);
validatePlanCacheSize(paramDefaultValue);

// Invalid values throw the same error.
const paramIncorrectValue = "100KB";
setPlanCacheSizeThrows(paramIncorrectValue, errorCode);
validatePlanCacheSize(paramDefaultValue);

MongoRunner.stopMongod(mongod);
})();
