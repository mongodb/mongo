/*
 * Rudimentary models for our core property tests.
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// .oneof() arguments are ordered from least complex to most, since fast-check uses this ordering to
// shrink.
export const scalarArb = fc.oneof(fc.integer({min: -30, max: 30}).map(i => NumberInt(i)),
                                  fc.boolean(),
                                  // Strings starting with `$` can be confused with fields.
                                  fc.string().filter(s => !s.startsWith('$')),
                                  fc.date(),
                                  fc.constant(null));

export const fieldArb = fc.constantFrom('t', 'm', 'm.m1', 'm.m2', 'a', 'b', 'array');
export const assignableFieldArb = fc.constantFrom('m', 't', 'a', 'b');
