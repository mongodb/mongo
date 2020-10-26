// Helper functions for testing time-series collections.

var TimeseriesTest = class {
    /**
     * Returns whether time-series collections are supported.
     */
    static timeseriesCollectionsEnabled(conn) {
        return assert
            .commandWorked(conn.adminCommand({getParameter: 1, featureFlagTimeSeriesCollection: 1}))
            .featureFlagTimeSeriesCollection.value;
    }

    /**
     * Adjusts the values in 'fields' by a random amount.
     * Ensures that the new values stay in the range [0, 100].
     */
    static updateUsages(fields) {
        for (const field in fields) {
            fields[field] += Math.round(Random.genNormal(0, 1));
            fields[field] = Math.max(fields[field], 0);
            fields[field] = Math.min(fields[field], 100);
        }
    }

    /**
     * Returns a random element from an array.
     */
    static getRandomElem(arr) {
        return arr[Random.randInt(arr.length)];
    }

    /**
     * Generates time-series data based on the TSBS document-per-event format.
     *
     * https://github.com/timescale/tsbs/blob/7508b34755e05f55a14ec4bac2913ae758b4fd78/cmd/tsbs_generate_data/devops/cpu.go
     * https://github.com/timescale/tsbs/blob/7508b34755e05f55a14ec4bac2913ae758b4fd78/cmd/tsbs_generate_data/devops/host.go
     */
    static generateHosts(numHosts) {
        const hosts = new Array(numHosts);

        const getRandomUsage = function() {
            return Random.randInt(101);
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
                    arch: TimeseriesTest.getRandomElem(["x64", "x86"]),
                    datacenter: TimeseriesTest.getRandomElem(dataCenters[regionIndex]),
                    hostname: "host_" + i,
                    os: TimeseriesTest.getRandomElem(
                        ["Ubuntu15.10", "Ubuntu16.10", "Ubuntu16.04LTS"]),
                    rack: Random.randInt(100).toString(),
                    region: regions[regionIndex],
                    service: Random.randInt(20).toString(),
                    service_environment:
                        TimeseriesTest.getRandomElem(["production", "staging", "test"]),
                    service_version: Random.randInt(2).toString(),
                    team: TimeseriesTest.getRandomElem(["CHI", "LON", "NYC", "SF"]),
                }

            };
        }

        return hosts;
    }
};
