// Tests setting connection establishment server parameters

function getIngressConnectionEstablishmentRatePerSec(mongo) {
    return mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentRatePerSec: 1})
        .ingressConnectionEstablishmentRatePerSec;
}

function getingressConnectionEstablishmentBurstCapacitySecs(mongo) {
    return mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentBurstCapacitySecs: 1})
        .ingressConnectionEstablishmentBurstCapacitySecs;
}

function getIngressConnectionEstablishmentMaxQueueDepthDefault(mongo) {
    return mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentMaxQueueDepth: 1})
        .ingressConnectionEstablishmentMaxQueueDepth;
}

// Check the default values are used if not set explicitly
let mongo = MongoRunner.runMongod();
const maxInt32 = Math.pow(2, 31) - 1;

let ingressConnectionEstablishmentRatePerSecDefault =
    getIngressConnectionEstablishmentRatePerSec(mongo);
assert.eq(ingressConnectionEstablishmentRatePerSecDefault, maxInt32);

let ingressConnectionEstablishmentBurstCapacitySecsDefault =
    getingressConnectionEstablishmentBurstCapacitySecs(mongo);
assert.eq(ingressConnectionEstablishmentBurstCapacitySecsDefault, Number.MAX_VALUE);

// There are infinite tokens available by default.
let availableTokens =
    mongo.adminCommand({serverStatus: 1}).queues.ingressSessionEstablishment.totalAvailableTokens;
assert.eq(availableTokens, Infinity);

let ingressConnectionEstablishmentMaxQueueDepthDefault =
    getIngressConnectionEstablishmentMaxQueueDepthDefault(mongo);
assert.eq(ingressConnectionEstablishmentMaxQueueDepthDefault, 0);

// Check that each parameter can be set during runtime
const runtimeSetValue = 10;
assert.commandWorked(mongo.adminCommand({
    setParameter: 1,
    ingressConnectionEstablishmentRatePerSec: runtimeSetValue,
    ingressConnectionEstablishmentBurstCapacitySecs: runtimeSetValue,
    ingressConnectionEstablishmentMaxQueueDepth: runtimeSetValue
}));

let ingressConnectionEstablishmentRatePerSec = getIngressConnectionEstablishmentRatePerSec(mongo);
assert.neq(ingressConnectionEstablishmentRatePerSec,
           ingressConnectionEstablishmentMaxQueueDepthDefault);
assert.eq(ingressConnectionEstablishmentRatePerSec, runtimeSetValue);

let ingressConnectionEstablishmentBurstCapacitySecs =
    getingressConnectionEstablishmentBurstCapacitySecs(mongo);
assert.neq(ingressConnectionEstablishmentBurstCapacitySecs,
           ingressConnectionEstablishmentBurstCapacitySecsDefault);
assert.eq(ingressConnectionEstablishmentBurstCapacitySecs, runtimeSetValue);

let ingressConnectionEstablishmentMaxQueueDepth =
    mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentMaxQueueDepth: 1})
        .ingressConnectionEstablishmentMaxQueueDepth;
assert.eq(ingressConnectionEstablishmentMaxQueueDepth, runtimeSetValue);

MongoRunner.stopMongod(mongo);

// Check that each parameter can be set at start up
const startupSetValue = 50;
mongo = MongoRunner.runMongod({
    setParameter: {
        ingressConnectionEstablishmentRatePerSec: startupSetValue,
        ingressConnectionEstablishmentBurstCapacitySecs: startupSetValue,
        ingressConnectionEstablishmentMaxQueueDepth: startupSetValue
    }
});

ingressConnectionEstablishmentRatePerSec = getIngressConnectionEstablishmentRatePerSec(mongo);
assert.eq(ingressConnectionEstablishmentRatePerSec, startupSetValue);

ingressConnectionEstablishmentBurstCapacitySecs =
    getingressConnectionEstablishmentBurstCapacitySecs(mongo);
assert.eq(ingressConnectionEstablishmentBurstCapacitySecs, startupSetValue);

ingressConnectionEstablishmentMaxQueueDepth =
    mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentMaxQueueDepth: 1})
        .ingressConnectionEstablishmentMaxQueueDepth;
assert.eq(ingressConnectionEstablishmentMaxQueueDepth, startupSetValue);

MongoRunner.stopMongod(mongo);

// Check that each parameter can be set from a string
const stringSetValue = 100;

mongo = MongoRunner.runMongod(
    {setParameter: 'ingressConnectionEstablishmentRatePerSec=' + stringSetValue});
ingressConnectionEstablishmentRatePerSec = getIngressConnectionEstablishmentRatePerSec(mongo);
assert.eq(ingressConnectionEstablishmentRatePerSec, stringSetValue);
MongoRunner.stopMongod(mongo);

mongo = MongoRunner.runMongod(
    {setParameter: 'ingressConnectionEstablishmentBurstCapacitySecs=' + stringSetValue});
ingressConnectionEstablishmentBurstCapacitySecs =
    getingressConnectionEstablishmentBurstCapacitySecs(mongo);
assert.eq(ingressConnectionEstablishmentBurstCapacitySecs, stringSetValue);
MongoRunner.stopMongod(mongo);

mongo = MongoRunner.runMongod(
    {setParameter: 'ingressConnectionEstablishmentMaxQueueDepth=' + stringSetValue});
ingressConnectionEstablishmentMaxQueueDepth =
    mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentMaxQueueDepth: 1})
        .ingressConnectionEstablishmentMaxQueueDepth;
assert.eq(ingressConnectionEstablishmentMaxQueueDepth, stringSetValue);
MongoRunner.stopMongod(mongo);

// Make sure that increasing the burstSize results in increasing the available tokens in the bucket
const marginOfError = 5;  // +/- 5 tokens just in case there is rounding errors/clock skew
const refreshRate = 10000;
const initialBurstCapacitySecs = 0.5;
mongo = MongoRunner.runMongod({
    setParameter: {
        ingressConnectionEstablishmentRatePerSec: refreshRate,
        ingressConnectionEstablishmentBurstCapacitySecs: initialBurstCapacitySecs,
    }
});
let initialTokens =
    mongo.adminCommand({serverStatus: 1}).queues.ingressSessionEstablishment.totalAvailableTokens;
const targetTokens = refreshRate * initialBurstCapacitySecs;
assert.between(targetTokens - marginOfError, initialTokens, targetTokens + marginOfError);

const newBurstCapacitySecs = 2;
const newTargetTokens = refreshRate * newBurstCapacitySecs;
assert.commandWorked(mongo.adminCommand(
    {setParameter: 1, ingressConnectionEstablishmentBurstCapacitySecs: newBurstCapacitySecs}));
assert.soon(() => {
    const newTokens = mongo.adminCommand({serverStatus: 1})
                          .queues.ingressSessionEstablishment.totalAvailableTokens;
    jsTestLog("New tokens: " + newTokens);
    return newTargetTokens - marginOfError <= newTokens &&
        newTokens <= newTargetTokens + marginOfError;
});

MongoRunner.stopMongod(mongo);
