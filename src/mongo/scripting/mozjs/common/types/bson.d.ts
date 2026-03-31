// type declarations for bson.h

declare function bsonWoCompare(a, b): number;
declare function bsonUnorderedFieldsCompare(a, b): number;
declare function bsonBinaryEqual(a, b): number;
declare function bsonObjToArray(a, b): number;
declare function bsonToBase64(a): string;

/**
 * Get a read-only wrapper around a BSON object that throws an exception if there is an attempt to
 * assign a value to one of its properties or one of its sub-document's properties.
 *
 * The immutable wrapper will never recompute the underlying BSON, unlike the default wrapper, which
 * can do that even in when there are no writes to the object.
 *
 * Any tests that use 'bsonBinaryEqual' to validate BSON responses from the server should
 * exclusively use immutable wrappers when accessing those values to ensure that the comparison uses
 * the original BSON.
 */
declare function bsonGetImmutable(a): any;
