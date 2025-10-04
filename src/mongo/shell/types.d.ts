// type declarations for types.js

declare class BSONAwareMap {}
declare class ISODate {}
declare function isNumber()
declare function isObject()
declare function isString()
declare function printjson()
declare function printjsononeline()
declare function toJsonForLog()

/**
 * Convert a value to a json-formatted string.
 * 
 * @param {*} object to be converted to json
 * @param {string} [indent] character to use as the "indent" when pretty-printed
 * @param {*} [nolint] Prints a oneliner when true, otherwise uses multiple lines and indents.
 * @param {number} [depth] Depth of nested fields to prints (default 0)
 * @param {boolean} [sortKeys] Whether or not to print keys in sorted order.
 */
declare function tojson(obj, indent: string = "", nolint: any, depth: number = 0, sortKeys: boolean)
declare function tojsonObject()
declare function tojsononeline()
