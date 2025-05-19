// Tests setting connection establishment server parameters

function getIngressConnectionEstablishmentRatePerSec(mongo) {
    return mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentRatePerSec: 1})
        .ingressConnectionEstablishmentRatePerSec;
}

function getIngressConnectionEstablishmentBurstSize(mongo) {
    return mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentBurstSize: 1})
        .ingressConnectionEstablishmentBurstSize;
}

function getIngressConnectionEstablishmentMaxQueueDepthDefault(mongo) {
    return mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentMaxQueueDepth: 1})
        .ingressConnectionEstablishmentMaxQueueDepth;
}

// Check the default values are used if not set explicitly
let mongo = MongoRunner.runMongod();

let ingressConnectionEstablishmentRatePerSecDefault =
    getIngressConnectionEstablishmentRatePerSec(mongo);
assert.eq(ingressConnectionEstablishmentRatePerSecDefault, Number.MAX_VALUE);

let ingressConnectionEstablishmentBurstSizeDefault =
    getIngressConnectionEstablishmentBurstSize(mongo);
assert.eq(ingressConnectionEstablishmentBurstSizeDefault, Number.MAX_VALUE);

let ingressConnectionEstablishmentMaxQueueDepthDefault =
    getIngressConnectionEstablishmentMaxQueueDepthDefault(mongo);
assert.eq(ingressConnectionEstablishmentMaxQueueDepthDefault, 0);

// Check that each parameter can be set during runtime
const runtimeSetValue = 10;
assert.commandWorked(mongo.adminCommand({
    setParameter: 1,
    ingressConnectionEstablishmentRatePerSec: runtimeSetValue,
    ingressConnectionEstablishmentBurstSize: runtimeSetValue,
    ingressConnectionEstablishmentMaxQueueDepth: runtimeSetValue
}));

let ingressConnectionEstablishmentRatePerSec = getIngressConnectionEstablishmentRatePerSec(mongo);
assert.neq(ingressConnectionEstablishmentRatePerSec,
           ingressConnectionEstablishmentMaxQueueDepthDefault);
assert.eq(ingressConnectionEstablishmentRatePerSec, runtimeSetValue);

let ingressConnectionEstablishmentBurstSize = getIngressConnectionEstablishmentBurstSize(mongo);
assert.neq(ingressConnectionEstablishmentBurstSize, ingressConnectionEstablishmentBurstSizeDefault);
assert.eq(ingressConnectionEstablishmentBurstSize, runtimeSetValue);

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
        ingressConnectionEstablishmentBurstSize: startupSetValue,
        ingressConnectionEstablishmentMaxQueueDepth: startupSetValue
    }
});

ingressConnectionEstablishmentRatePerSec = getIngressConnectionEstablishmentRatePerSec(mongo);
assert.eq(ingressConnectionEstablishmentRatePerSec, startupSetValue);

ingressConnectionEstablishmentBurstSize = getIngressConnectionEstablishmentBurstSize(mongo);
assert.eq(ingressConnectionEstablishmentBurstSize, startupSetValue);

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
    {setParameter: 'ingressConnectionEstablishmentBurstSize=' + stringSetValue});
ingressConnectionEstablishmentBurstSize = getIngressConnectionEstablishmentBurstSize(mongo);
assert.eq(ingressConnectionEstablishmentBurstSize, stringSetValue);
MongoRunner.stopMongod(mongo);

mongo = MongoRunner.runMongod(
    {setParameter: 'ingressConnectionEstablishmentMaxQueueDepth=' + stringSetValue});
ingressConnectionEstablishmentMaxQueueDepth =
    mongo.adminCommand({getParameter: 1, ingressConnectionEstablishmentMaxQueueDepth: 1})
        .ingressConnectionEstablishmentMaxQueueDepth;
assert.eq(ingressConnectionEstablishmentMaxQueueDepth, stringSetValue);
MongoRunner.stopMongod(mongo);
