/**
 * Inserts time series data based on the TSBS document-per-event format.
 *
 * https://github.com/timescale/tsbs/blob/7508b34755e05f55a14ec4bac2913ae758b4fd78/cmd/tsbs_generate_data/devops/cpu.go
 * https://github.com/timescale/tsbs/blob/7508b34755e05f55a14ec4bac2913ae758b4fd78/cmd/tsbs_generate_data/devops/host.go
 */
(function() {
"use strict";

const coll = db.getCollection(jsTestName());
Random.setRandomSeed();

const getRandomUsage = function() {
    return Random.randInt(101);
};

const updateUsages = function(fields) {
    for (const field in fields) {
        fields[field] += Math.round(Random.genNormal(0, 1));
        fields[field] = Math.max(fields[field], 0);
        fields[field] = Math.min(fields[field], 100);
    }
};

const getRandomElem = function(arr) {
    return arr[Random.randInt(arr.length)];
};

const regions = [
    "ap-northeast-1",
    "ap-southeast-1",
    "ap-southeast-2",
    "eu-central-1",
    "eu-west-1",
    "sa-east-1",
    "us-east-1",
    "us-west-1",
    "us-west-2",
];

const dataCenters = [
    [
        "ap-northeast-1a",
        "ap-northeast-1c",
    ],
    [
        "ap-southeast-1a",
        "ap-southeast-1b",
    ],
    [
        "ap-southeast-2a",
        "ap-southeast-2b",
    ],
    [
        "eu-central-1a",
        "eu-central-1b",
    ],
    [
        "eu-west-1a",
        "eu-west-1b",
        "eu-west-1c",
    ],
    [
        "sa-east-1a",
        "sa-east-1b",
        "sa-east-1c",
    ],
    [
        "us-east-1a",
        "us-east-1b",
        "us-east-1c",
        "us-east-1e",
    ],
    [
        "us-west-1a",
        "us-west-1b",
    ],
    [
        "us-west-2a",
        "us-west-2b",
        "us-west-2c",
    ],
];

const hosts = new Array(10);
for (let i = 0; i < hosts.length; i++) {
    const regionIndex = Random.randInt(regions.length);
    hosts[i] = {
        fields: {
            usage_guest: getRandomUsage(),
            usage_guest_nice: getRandomUsage(),
            usage_idle: getRandomUsage(),
            usage_iowait: getRandomUsage(),
            usage_irq: getRandomUsage(),
            usage_nice: getRandomUsage(),
            usage_softirq: getRandomUsage(),
            usage_steal: getRandomUsage(),
            usage_system: getRandomUsage(),
            usage_user: getRandomUsage(),
        },
        tags: {
            arch: getRandomElem(["x64", "x86"]),
            datacenter: getRandomElem(dataCenters[regionIndex]),
            hostname: "host_" + i,
            os: getRandomElem(["Ubuntu15.10", "Ubuntu16.10", "Ubuntu16.04LTS"]),
            rack: Random.randInt(100).toString(),
            region: regions[regionIndex],
            service: Random.randInt(20).toString(),
            service_environment: getRandomElem(["production", "staging", "test"]),
            service_version: Random.randInt(2).toString(),
            team: getRandomElem(["CHI", "LON", "NYC", "SF"]),
        }
    };
}

for (let i = 0; i < 100; i++) {
    const host = getRandomElem(hosts);
    updateUsages(host.fields);

    assert.commandWorked(coll.insert({
        measurement: "cpu",
        time: ISODate(),
        fields: host.fields,
        tags: host.tags,
    }));
}
})();