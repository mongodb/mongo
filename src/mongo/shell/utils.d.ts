// type declarations for utils.js

declare {
    /**
     * Returns the name of the current jsTest to be used as an identifier.
     * This may be prefixed and/or hashed to improve traceability.
     * 
     * @example
     * const coll = db[jsTestName()];
     */
    export function jsTestName(): string
}
