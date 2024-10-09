// This lists all supported releases and needs to be kept up to date as versions are added and
// dropped.
// TODO SERVER-76166: Programmatically generate list of LTS versions.
export const allLtsVersions = [
    {binVersion: '4.4', featureCompatibilityVersion: '4.4', testCollection: 'four_four'},
    {binVersion: '5.0', featureCompatibilityVersion: '5.0', testCollection: 'five_zero'},
    {binVersion: '6.0', featureCompatibilityVersion: '6.0', testCollection: 'six_zero'},
    {binVersion: '7.0', featureCompatibilityVersion: '7.0', testCollection: 'seven_zero'},
    {binVersion: 'last-lts', featureCompatibilityVersion: lastLTSFCV, testCollection: 'last_lts'},
    {
        binVersion: 'last-continuous',
        featureCompatibilityVersion: lastContinuousFCV,
        testCollection: 'last_continuous'
    },
    {binVersion: 'latest', featureCompatibilityVersion: latestFCV, testCollection: 'latest'},
];
