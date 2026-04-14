const UINT64_MASK = 0xffffffffffffffffn;

/**
 * JSON.stringify() that is BigInt-safe, as BigInt values can not
 * be passed directly to JSON.stringify() without raising an exception.
 */
function jsonStringifyBigIntSafe(value) {
    return JSON.stringify(value, (_key, v) => (typeof v === "bigint" ? v.toString() : v));
}

/**
 * Recursively canonicalize a value so that object key order and array element order do
 * not affect the serialized form.
 */
function canonicalizeForChecksum(value) {
    if (value === null) {
        return null;
    }

    if (typeof value !== "object") {
        return value;
    }

    if (Array.isArray(value)) {
        const outputArray = value.map(canonicalizeForChecksum);
        return outputArray.toSorted((a, b) => JSON.stringify(a).localeCompare(JSON.stringify(b)));
    }

    const proto = Object.getPrototypeOf(value);
    if (proto !== null && proto !== Object.prototype) {
        assert(false, `Object type ${proto} not supported by canonicalizeForChecksum().`);
    }

    // This is a plain dictionary. Sort the keys and canonicalize the values.
    const outputDict = {};
    for (const k of Object.keys(value).sort()) {
        outputDict[k] = canonicalizeForChecksum(value[k]);
    }
    return outputDict;
}

/**
 * Hash a string using the FNV-1a function
 * https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
 */
function fnv1a64String(s) {
    const FNV1A64_OFFSET = 14695981039346656037n;
    const FNV1A64_PRIME = 1099511628211n;

    let h = FNV1A64_OFFSET;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        h ^= BigInt(c & 0xff);
        h = (h * FNV1A64_PRIME) & UINT64_MASK;
        h ^= BigInt((c >> 8) & 0xff);
        h = (h * FNV1A64_PRIME) & UINT64_MASK;
    }
    return h;
}

/**
 * Order-insensitive 64-bit checksum of a resultset.
 */
export function resultsetChecksum(resultset) {
    // Remove all BigInt, Date, Time, etc. objects from the resultset
    // by converting the resultset to EJSON and back.
    const json = JSON.parse(jsonStringifyBigIntSafe(resultset));

    // Canonicalize the resultset by sorting all keys and values.
    const canonical = canonicalizeForChecksum(json);

    // Compute the checksum using the FNV-1a 64-bit hash function.
    const checksum = fnv1a64String(JSON.stringify(canonical)) & UINT64_MASK;
    return checksum.toString(16).padStart(16, "0");
}
