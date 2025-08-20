/**
 * Unit tests for utilities in search_e2e_utils.js.
 */
import {
    assertDocArrExpectedFuzzy,
    defaultFuzzingStrategy,
    defaultTolerancePercentage,
    FuzzingStrategy,
} from "jstests/with_mongot/e2e/lib/search_e2e_utils.js";

function assertResultsExpectedFuzzyTest() {
    // Helper function for testing when an assertion within 'assertResultsExpectedFuzzy()' function
    // is expected.
    function assertionExpected(expected,
                               actual,
                               tolerance = defaultTolerancePercentage,
                               fuzzing = defaultFuzzingStrategy) {
        let assertionRaised = false;
        try {
            assertDocArrExpectedFuzzy(expected, actual, undefined, tolerance, fuzzing);
        } catch (error) {
            assertionRaised = true;
        }
        assert(assertionRaised, "assertion was expected to be raised, but wasnt");
    }

    // Test that only arrays are accepted.
    {
        const expected = {"hello": "world"};
        const actual = {"hello": "world"};
        assertionExpected(expected, actual);
    }

    // Trival success case, empty arrays must be equal.
    { assertDocArrExpectedFuzzy([], []); }

    // Test that tolerance percentages within range [0, 1] are accepted.
    {
        assertDocArrExpectedFuzzy([], [], 0);
        assertDocArrExpectedFuzzy([], [], 0.5);
        assertDocArrExpectedFuzzy([], [], 1);
    }

    // Test that tolerance percentages out of range [0, 1] are not accepted.
    {
        assertionExpected([], [], -0.5);
        assertionExpected([], [], 1.5);
    }

    // Test arrays of different lengths are not accepted.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            }
        ];
        assertionExpected(expected, actual);
    }

    // Test that any array that contains a doc with no "_id" key fails.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "id": 1,  // Note no "_id" here.
            },
            {
                "_id": 2,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            }
        ];
        assertionExpected(expected, actual);
    }
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "id": 1,  // Note no "_id" here.
            },
            {
                "_id": 2,
            }
        ];
        assertionExpected(expected, actual);
    }

    // Test that when arrays are equal, but duplicate keys exist, it is not accepted.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 1,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 1,
            }
        ];
        assertionExpected(expected, actual);
    }

    // Test that arrays of the same length, but not all matching doc id's are not accepted.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 3,  // Note 3, not 2.
            }
        ];
        assertionExpected(expected, actual);
    }

    // Every key in the actual array is in expected array, but actual array has duplicates;
    // not allowed.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 1,
            }
        ];
        assertionExpected(expected, actual);
    }

    // Simple (non-trival) success case, all docs are in exact order as expected.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            }
        ];
        assertDocArrExpectedFuzzy(expected, actual);
        // Should also work with 0 tolerance in either fuzzing strategy.
        assertDocArrExpectedFuzzy(expected, actual, 0, FuzzingStrategy.EnforceTolerancePerDoc);
        assertDocArrExpectedFuzzy(expected, actual, 0, FuzzingStrategy.ShareToleranceAcrossDocs);
    }

    // Test that a doc array in the correct order, but with non-matching fields is not accepted.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
                "hello": "world",
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
                "hello": "world!",  // Note non-matching field.
            }
        ];
        assertionExpected(expected, actual);
    }

    // Test that doc array in correct order, with same fields in a different order passes.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
                "field1": "value1",
                "field2": "value2",
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,  // Equivalant document with fields in a different order.
                "field2": "value2",
                "field1": "value1",
            }
        ];
        assertDocArrExpectedFuzzy(expected, actual);
    }

    // Test that a simple swap of two is accepted with proper tolerance.
    // Array of length 5 with a tolerance of 0.2 should allow for a position off of (5 * 0.2) = 1.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            },
            {
                "_id": 3,
            },
            {
                "_id": 4,
            },
            {
                "_id": 5,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 3,
            },
            {
                "_id": 2,  // Note swap of 2 and 3.
            },
            {
                "_id": 4,
            },
            {
                "_id": 5,
            }
        ];
        assertDocArrExpectedFuzzy(
            expected, actual, undefined, 0.2, FuzzingStrategy.EnforceTolerancePerDoc);
    }

    // Test that with the same array length and tolerance, a swap of 2 positions is not accepted.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            },
            {
                "_id": 3,
            },
            {
                "_id": 4,
            },
            {
                "_id": 5,
            }
        ];
        const actual = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 4,
            },
            {
                "_id": 3,
            },
            {
                "_id": 2,  // Note swap of 2 and 4.
            },
            {
                "_id": 5,
            }
        ];
        assertionExpected(expected, actual, 0.2, FuzzingStrategy.EnforceTolerancePerDoc);
        // However, if tolerance is shared across docs, this should be accepted.
        assertDocArrExpectedFuzzy(
            expected, actual, undefined, 0.2, FuzzingStrategy.ShareToleranceAcrossDocs);
    }

    // Test that shared tolerance fuzzing strategy passes on a tight tolerance with a single outlier
    // and an otherwise in-order array.
    // 10 elements with a tolerance of 0.1 is a global positional tolerance of (10 * 0.1) * 10 = 10.
    // Two elements swapped at 5 positions away should just at the global tolerance.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            },
            {
                "_id": 3,
            },
            {
                "_id": 4,
            },
            {
                "_id": 5,
            },
            {
                "_id": 6,
            },
            {
                "_id": 7,
            },
            {
                "_id": 8,
            },
            {
                "_id": 9,
            }
        ];
        const actual = [
            {
                "_id": 5,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            },
            {
                "_id": 3,
            },
            {
                "_id": 4,
            },
            {
                "_id": 0,  // Outlier swap 5 positions away.
            },
            {
                "_id": 6,
            },
            {
                "_id": 7,
            },
            {
                "_id": 8,
            },
            {
                "_id": 9,
            }
        ];
        assertDocArrExpectedFuzzy(
            expected, actual, undefined, 0.1, FuzzingStrategy.ShareToleranceAcrossDocs);
    }

    // Same as above, but shows any other swap at any distance will cause global cap to be reached.
    {
        const expected = [
            {
                "_id": 0,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            },
            {
                "_id": 3,
            },
            {
                "_id": 4,
            },
            {
                "_id": 5,
            },
            {
                "_id": 6,
            },
            {
                "_id": 7,
            },
            {
                "_id": 8,
            },
            {
                "_id": 9,
            }
        ];
        const actual = [
            {
                "_id": 5,
            },
            {
                "_id": 1,
            },
            {
                "_id": 2,
            },
            {
                "_id": 3,
            },
            {
                "_id": 4,
            },
            {
                "_id": 0,  // Same outlier swap 5 positions away.
            },
            {
                "_id": 6,
            },
            {
                "_id": 7,
            },
            {
                "_id": 9,
            },
            {
                "_id": 8,  // Additional minimal swap of 8 and 9. Causes failure.
            }
        ];
        assertionExpected(expected, actual, 0.1, FuzzingStrategy.ShareToleranceAcrossDocs);
    }
}

// Run tests.
assertResultsExpectedFuzzyTest();
