/*
 * Rudimentary models for our core property tests.
 */
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

/*
 * In these arbitraries, we use stratified sampling to increase the likelihood that interesting
 * cases are found.
 * For example, integers has interesting cases at the minimimum, -1, 0, 1, and maximum. Stratifying
 * the small range [-1, 1] also encourages the model to create filters that match documents.
 * Generating {$match: {a: 1}} against document {a: 1} is more likely.
 */
const kInt32Min = -2147483648;
const kInt32Max = +2147483647;
export const intArb = oneof(
                          fc.integer({min: -1, max: +1}),                // tiny
                          fc.integer({min: -20, max: +20}),              // smallish
                          fc.integer({min: kInt32Min, max: kInt32Max}),  // full range
                          fc.constantFrom(kInt32Min, kInt32Max)          // interesting corner cases
                          )
                          .map(i => NumberInt(i));

/*
 * Stratify with regular characters, unicode, ascii, and null byte. Null byte is a special case
 * because it can indicate the end of a string in string implementations, so it may need special
 * logic.
 */
const nullByte =
    fc.constantFrom('\0', '\x00', '\x01', '\x02', '\x03', '\x08', '\x18', '\x28', '\xff');
const charArb = oneof(fc.base64(), fc.unicode(), fc.ascii(), nullByte).filter(c => c !== '$');
const stringArb = fc.stringOf(charArb, {maxLength: 3});

// ValidateCollections fails if a partial index with a filter involving Date(year=0) exists. This
// year=0 behavior is accepted as a part of the PyMongo BSON library. To avoid false positives with
// the ValidateCollections hook, we make the minimum date year=1
const kMinDate = ISODate("0001-01-01T00:00:00Z");
const kMaxDate = ISODate("9999-12-31T23:59:59.999Z");
export const dateArb = oneof(
    fc.date({min: new Date(-1), max: new Date(1)}),      // tiny
    fc.date({min: new Date(-100), max: new Date(100)}),  // smallish
    fc.date({min: kMinDate, max: kMaxDate}),             // full range
    fc.constantFrom(kMinDate, kMaxDate)                  // interesting corner cases
);

// .oneof() arguments are ordered from least complex to most, since fast-check uses this ordering to
// shrink.
export const scalarArb = oneof(intArb, fc.boolean(), stringArb, dateArb, fc.constant(null));

export const fieldArb = fc.constantFrom('a', 'b', 't', 'm', '_id', 'm.m1', 'm.m2', 'array');
export const dollarFieldArb = fieldArb.map(f => "$" + f);
export const assignableFieldArb = fc.constantFrom('a', 'b', 't', 'm');

export const leafParametersPerFamily = 10;
export class LeafParameter {
    constructor(concreteValues) {
        this.concreteValues = concreteValues;
    }
}

export const leafParameterArb =
    fc.array(scalarArb, {minLength: 1, maxLength: leafParametersPerFamily}).map((constants) => {
        // In the leaves of the query family, we generate an object with a list of constants to
        // place.
        return new LeafParameter(constants);
    });
