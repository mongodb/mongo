/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

TEST_CASE("Binary Search String: WT_BINARY_SEARCH_STRING", "[search]")
{
    bool found;

    SECTION("Key exists in the array")
    {
        const char *array[] = {"apple", "banana", "cherry", "date", "elderberry"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("banana", array, n, found);
        REQUIRE(found == true);

        WT_BINARY_SEARCH_STRING("elderberry", array, n, found);
        REQUIRE(found == true);

        WT_BINARY_SEARCH_STRING("apple", array, n, found);
        REQUIRE(found == true);
    }

    SECTION("Key does not exist in the array")
    {
        const char *array[] = {"apple", "banana", "cherry", "date", "elderberry"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("grape", array, n, found);
        REQUIRE(found == false);

        WT_BINARY_SEARCH_STRING("kiwi", array, n, found);
        REQUIRE(found == false);

        WT_BINARY_SEARCH_STRING("fig", array, n, found);
        REQUIRE(found == false);
    }

    SECTION("Empty array")
    {
        const char *array[] = {NULL};
        const uint32_t n = 0;

        WT_BINARY_SEARCH_STRING("apple", array, n, found);
        REQUIRE(found == false);

        WT_BINARY_SEARCH_STRING("banana", array, n, found);
        REQUIRE(found == false);

        WT_BINARY_SEARCH_STRING("cherry", array, n, found);
        REQUIRE(found == false);
    }

    SECTION("Single element array")
    {
        const char *array[] = {"apple"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("apple", array, n, found);
        REQUIRE(found == true);

        WT_BINARY_SEARCH_STRING("banana", array, n, found);
        REQUIRE(found == false);
    }

    SECTION("Array with duplicate elements")
    {
        const char *array[] = {
          "apple", "banana", "banana", "cherry", "date", "elderberry", "elderberry"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("banana", array, n, found);
        REQUIRE(found == true);

        WT_BINARY_SEARCH_STRING("elderberry", array, n, found);
        REQUIRE(found == true);

        WT_BINARY_SEARCH_STRING("fig", array, n, found);
        REQUIRE(found == false);
    }

    SECTION("Key exists in the array and is the first or last element")
    {
        const char *array[] = {"apple", "banana", "cherry", "date", "elderberry"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("apple", array, n, found);
        REQUIRE(found == true);

        WT_BINARY_SEARCH_STRING("elderberry", array, n, found);
        REQUIRE(found == true);
    }

    SECTION("Prefixes in array and search for full string")
    {
        const char *array[] = {"apple", "banana", "cherry", "date", "elderberry"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("apples", array, n, found);
        REQUIRE(found == false);
    }

    SECTION("Full strings in array and search for prefix")
    {
        const char *array[] = {"apples", "bananas", "cherries", "dates", "elderberries"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("apple", array, n, found);
        REQUIRE(found == false);
    }

    SECTION("Empty string")
    {
        const char *array[] = {"apple", "banana", "cherry", "date", "elderberry"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("", array, n, found);
        REQUIRE(found == false);
    }

    SECTION("Empty string in array with empty string")
    {
        /* Empty strings will be considered lexically smaller than any non-empty string. */
        const char *array[] = {"", "apple", "banana", "cherry", "date", "elderberry"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("", array, n, found);
        REQUIRE(found == true);
    }

    SECTION("Empty string in array")
    {
        /* Empty strings will be considered lexically smaller than any non-empty string. */
        const char *array[] = {"", "apple", "banana", "cherry", "date", "elderberry"};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("apples", array, n, found);
        REQUIRE(found == false);
    }

    SECTION("Empty string in array with only empty strings")
    {
        const char *array[] = {"", "", "", "", ""};
        const uint32_t n = sizeof(array) / sizeof(array[0]);

        WT_BINARY_SEARCH_STRING("", array, n, found);
        REQUIRE(found == true);
    }
}
