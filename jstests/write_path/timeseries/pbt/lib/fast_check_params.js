export const getFcParams = () => {
    return TestData.fastCheckParameters || {};
};

export const getFcAssertArgs = () => {
    const fcParams = TestData.fastCheckParameters || {};
    const isSeeded = typeof fcParams.seed === "number";
    const fcAssertArgs = {
        numRuns: fcParams.numRuns || 50,
        seed: fcParams.seed,
        endOnFailure: fcParams.endOnFailure !== undefined ? fcParams.endOnFailure : isSeeded,
        path: fcParams.path,
    };

    jsTest.log.info("FAST_CHECK_PARAMETERS", {
        fastCheckParameters: Object.keys(fcParams).length > 0 ? fcParams : "not provided, will use defaults",
    });

    return fcAssertArgs;
};
