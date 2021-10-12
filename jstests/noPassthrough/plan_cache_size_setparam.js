/**
 * Validates that the planCacheSize parameter is set correctly at startup and runtime.
 */

(function() {
"use strict";

const paramName = "planCacheSize";
const paramStartValue = "711MB";

let mongod = MongoRunner.runMongod({setParameter: `${paramName}=${paramStartValue}`});
let adminDb = mongod.getDB("admin");

function validatePlanCacheSize(paramValue) {
    let result = adminDb.runCommand({getParameter: 1, [paramName]: 1});
    assert.commandWorked(result);
    assert.eq(result[paramName], paramValue);
}

function setPlanCacheSize(paramValue, errorCode) {
    let result = adminDb.runCommand({setParameter: 1, [paramName]: paramValue});
    assert.commandWorked(result);
}

function setPlanCacheSizeThrows(paramValue, errorCode) {
    let result = adminDb.runCommand({setParameter: 1, [paramName]: paramValue});
    assert.commandFailedWithCode(result, errorCode);
}

// Validates that the parameter is set correctly at startup.
validatePlanCacheSize(paramStartValue);

// Validates that the parameter is set correctly in runtime.
const paramCorrectValue = "10%";
setPlanCacheSize(paramCorrectValue);
validatePlanCacheSize(paramCorrectValue);

// Validates that an incorrect value is not accepted.
const paramIncorrectValue = "100KB";
setPlanCacheSizeThrows(paramIncorrectValue, 6007012);
validatePlanCacheSize(paramCorrectValue);

MongoRunner.stopMongod(mongod);
})();
