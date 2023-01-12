// Small data generator for the purpose of developing the test framework.

const alphabet = "abcdefghijklmnopqrstuvwxyz";
const len = alphabet.length;

// Returns pseudo-random string where the symbols and the length are functions of the parameter n.
function genRandomString(n) {
    let strLen = n % 4 + 1;
    let str = "";
    let i = 0;
    while (i < strLen) {
        let idx = (100 * n + i++) % len;
        str = str + alphabet[idx];
    }
    return str;
}

const seedArray = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 17, 18, 19, 20];
const arrLen = seedArray.length;

// Returns pseudo-random array where the elements and the length are functions of the parameter n.
function genRandomArray(n) {
    let aLen = (7 * n) % 5 + 1;
    let start = (13 * n) % arrLen;
    return seedArray.slice(start, start + aLen);
}

/**
 * Returns documents for cardinality estimation tests.
 */

function getCEDocs() {
    return Array.from(
        {length: 10},
        (_, i) =>
            ({_id: i, a: i + 10, b: genRandomString(i), c: genRandomArray(i), mixed: i * 11}));
}

function getCEDocs1() {
    return Array.from({length: 10}, (_, i) => ({
                                        _id: i + 10,
                                        a: i + 25,
                                        b: genRandomString(i + 25),
                                        c: genRandomArray(i + 25),
                                        mixed: genRandomString(i + 3)
                                    }));
}
