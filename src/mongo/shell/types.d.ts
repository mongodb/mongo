// type declarations for types.js

declare class BSONAwareMap {}
declare class ISODate {}

/**
 * Returns true if `typeof` is "number"
 */
declare function isNumber(x: any): boolean

/**
 * Returns true if `typeof` is "object"
 */
declare function isObject(x: any): boolean

/**
 * Returns true if `typeof` is "string"
 */
declare function isString(x: any): boolean

/**
 * A convenience for print(tojson(x))
 * 
 * See {@link tojson} for more.
 */
declare function printjson(x: any)

/**
 * A convenience for print(tojsononeline(x))
 * 
 * See {@link tojsononeline} for more.
 */
declare function printjsononeline()

/**
 * Serializes the given argument 'x' to a valid JSON string suitable for logging.
 * 
 * The results of `toJsonForLog` and {@link tostrictjson} should be equal for BSON objects and arrays.
 * Unlike {@link tostrictjson}, `toJsonForLog` also accepts non-object types, recognizes recursive
 * objects, and provides more detailed serializations for commonly used JavaScript classes, for
 * instance:
 *  - Set instances serialize to `{"$set": [<elem1>,...]}`
 *  - Map instances serialize to `{"$map": [[<key1>, <value1>],...]}`
 *  - Errors instances serialize to `{"$error": "<error_message>"}`
 *
 * `toJsonForLog` must be used when serializing JavaScript values into JSON logs to adhere to the
 * format requirements.
 *
 * Unlike {@link tojson}, the result of `eval(toJsonForLog(x))` will not always evaluate into an object
 * equivalent to `x` and may throw a syntax error.
 */
declare function toJsonForLog(x): string

/**
 * Convert a value to a json-formatted string.
 * Serializes the input to a string that can be used to deserialize it with 'eval()'.
 * 
 * @param {*} value Value to be converted to json
 * @param {string} [indent] String to use as the "indent" when pretty-printed.
 *   * If nullish or empty ("") and `nolint` is false, a single tab character ("\t") is used.
 *   * When `nolint` is true, the specified `indent` is ignored and an empty indent ("") is used
 * @param {*} [nolint] Constructs a oneliner when true, otherwise uses multiple lines with indents.
 * The output will be implicitly converted to a oneliner when all of the following are true:
 *   1. nolint is true or undefined
 *   2. indent is nullish or empty
 *   3. the total length of the output is <80 characters.
 * @param {number} [depth] Value used for tracking internal recursion limits and incremental indenting (default 0).
 *   If you want to truncate recursion earlier, set `tojson.MAX_DEPTH` accordingly.
 * @param {boolean} [sortKeys] Whether or not to print keys in sorted order.
 *
 * @returns {string} The return value is not always a valid JSON string. Use {@link toJsonForLog} for valid JSON output
 * and for printing values into the logs.
 */
declare function tojson(val, indent: string = "", nolint: any, depth: number = 0, sortKeys: boolean = false): string

/**
 * Convert an object to a json-formatted string.
 * 
 * This is called implicitly by {@link tojson}.
 */
declare function tojsonObject(obj: Object, indent: string = "", nolint: any, depth: number = 0, sortKeys: boolean = false): string

/**
 * A convenience for tojson(x, "", true)
 * 
 * See {@link tojson} for more.
 */
declare function tojsononeline(x: any): string
