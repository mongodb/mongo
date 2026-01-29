// type declarations for types.js

/**
 * Map data structure that preserves BSON type information.
 * Used internally by the shell for handling BSON-specific types.
 */
declare class BSONAwareMap {}

/**
 * Create a Date object from an ISO 8601 date string.
 * More flexible than Date.parse() - allows optional separators and fractional seconds.
 *
 * @param isoDateStr ISO 8601 date string (e.g., "2024-01-15T10:30:00.123Z", "20240115"). Omit for current date.
 * @returns Date object
 * @example
 * ISODate("2024-01-15T10:30:00Z")
 * ISODate("2024-01-15") // midnight UTC
 * ISODate() // current date/time
 */
declare function ISODate(isoDateStr?: string): Date

/**
 * Check if a value is a number.
 * Type guard function for TypeScript type narrowing.
 *
 * @param x Value to check
 * @returns True if x is a number type
 */
declare function isNumber(x: any): x is number

/**
 * Check if a value is an object.
 * Type guard function for TypeScript type narrowing.
 *
 * Note: This returns true for arrays as well (arrays are objects in JavaScript).
 *
 * @param x Value to check
 * @returns True if x is an object type (including arrays and null)
 */
declare function isObject(x: any): x is object

/**
 * Check if a value is a string.
 * Type guard function for TypeScript type narrowing.
 *
 * @param x Value to check
 * @returns True if x is a string type
 */
declare function isString(x: any): x is string

/**
 * Print a value as formatted JSON to the console.
 * Uses multi-line formatting with indentation for readability.
 * 
 * A convenience for `print(tojson(x))`.
 *
 * @param x Value to print
 *
 * See {@link tojsononeline} for more.
 */
declare function printjson(x: any): void

/**
 * Print a value as single-line JSON to the console.
 * Useful for compact output in logs or when space is limited.
 *
 * @param x Value to print
 */
declare function printjsononeline(x: any): void

/**
 * Convert a value to JSON string suitable for structured logging.
 * 
 * The results of `toJsonForLog` and {@link tostrictjson} should be equal for BSON objects and arrays.
 * Unlike {@link tostrictjson}, `toJsonForLog` also accepts non-object types, recognizes recursive
 * objects, and provides more detailed serializations for commonly used JavaScript classes.
 * 
 * Handles special JavaScript types that standard JSON.stringify doesn't:
 * - undefined → {"$undefined": true}
 * - Error → {"$error": message}
 * - Map → {"$map": [...entries]}
 * - Set → {"$set": [...values]}
 * - Circular references → "[recursive]"
 *
 * @param x Value to convert
 * @returns JSON string suitable for logging systems
 * 
 * Unlike {@link tojson}, the result of `eval(toJsonForLog(x))` will not always evaluate into an object
 * equivalent to `x` and may throw a syntax error.
 */
declare function toJsonForLog(x: any): string

/**
 * Convert a value to a JSON-formatted string.
 * Serializes the input to a string that can be used to deserialize it with 'eval()'.
 *
 * Note: The output is not always valid standard JSON. MongoDB types like ObjectId, Date,
 * NumberLong, etc. are represented in MongoDB Extended JSON format for shell compatibility.
 *
 * @param val Value to be converted to JSON
 * @param indent Character or string to use as the indent when pretty-printed.
 *   * If nullish or empty ("") and `nolint` is false, a single tab character ("\t") is used.
 *   * When `nolint` is true, the specified `indent` is ignored and an empty indent ("") is used
 * @param nolint Constructs a oneliner when true, otherwise uses multiple lines with indents.
 * The output will be implicitly converted to a oneliner when all of the following are true:
 *   1. nolint is true or undefined
 *   2. indent is nullish or empty
 *   3. the total length of the output is <80 characters.
 * @param depth Value used for tracking internal recursion limits and incremental indenting (default 0).
 *   If you want to truncate recursion earlier, set `tojson.MAX_DEPTH` accordingly.
 * @param sortKeys If true, prints object keys in alphabetical order
 * @returns JSON string representation. The return value is not always a valid JSON string. Use {@link toJsonForLog} for valid JSON output
 * and for printing values into the logs.
 * @example
 * tojson({_id: ObjectId(), name: "Alice"})
 * // => '{\n  "_id": ObjectId("..."),\n  "name": "Alice"\n}'
 * tojson(doc, "", true)  // single-line output
 * tojson(doc, "\t")      // tab-indented
 */
declare function tojson(val, indent?: string = "", nolint?: any, depth?: number = 0, sortKeys?: boolean): string

/**
 * Convert an object to a json-formatted string.
 * Lower-level function used by {@link tojson}.
 */
declare function tojsonObject(obj: Object, indent: string = "", nolint: any, depth: number = 0, sortKeys: boolean = false): string

/**
 * Convert a value to a compact single-line JSON string.
 * Shorthand for tojson(x, "", true).
 * 
 * See {@link tojson} for more.
 */
declare function tojsononeline(x: any): string