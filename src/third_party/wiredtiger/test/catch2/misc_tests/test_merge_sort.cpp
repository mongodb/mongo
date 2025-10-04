#include <catch2/catch.hpp>

#include "wt_internal.h"

struct KV {
    int key;
    int value;
};

WT_SESSION_IMPL *unused_session = nullptr;

int
compare(WT_SESSION_IMPL *unused_session, const KV *a, const KV *b)
{
    WT_UNUSED(unused_session);
    return a->key - b->key;
}

TEST_CASE("WT_MERGE_SORT Tests", "[WT_MERGE_SORT]")
{
    SECTION("Test empty arrays")
    {
        KV **arr1 = nullptr;
        size_t size1 = 0;
        KV **arr2 = nullptr;
        size_t size2 = 0;
        KV *merged_arr[1];
        size_t merged_size = 0;
        WT_MERGE_SORT(
          unused_session, arr1, size1, arr2, size2, compare, 1, merged_arr, merged_size);
        REQUIRE(merged_size == 0);
        WT_UNUSED(merged_arr[1]);
    }

    SECTION("Test one empty array")
    {
        KV a{1, 10}, b{2, 20};
        KV *arr1[2] = {&a, &b};
        size_t size1 = 2;
        KV **arr2 = nullptr;
        size_t size2 = 0;
        KV *merged_arr[2];
        size_t merged_size = 0;
        WT_MERGE_SORT(
          unused_session, arr1, size1, arr2, size2, compare, 1, merged_arr, merged_size);
        REQUIRE(merged_size == 2);
        REQUIRE(merged_arr[0]->key == 1);
        REQUIRE(merged_arr[1]->key == 2);
    }

    SECTION("Test non overlapping arrays")
    {
        KV a{1, 10}, b{3, 30};
        KV c{2, 20}, d{4, 40};
        KV *arr1[2] = {&a, &b};
        KV *arr2[2] = {&c, &d};
        KV *merged_arr[4];
        size_t merged_size = 0;
        WT_MERGE_SORT(unused_session, arr1, 2, arr2, 2, compare, 1, merged_arr, merged_size);
        REQUIRE(merged_size == 4);
        REQUIRE(merged_arr[0]->key == 1);
        REQUIRE(merged_arr[1]->key == 2);
        REQUIRE(merged_arr[2]->key == 3);
        REQUIRE(merged_arr[3]->key == 4);
    }

    SECTION("Test with duplicates prefer_latest true")
    {
        KV a{2, 10}, c{3, 30};
        KV b{2, 20}, d{4, 40};
        KV *arr1[2] = {&a, &c};
        KV *arr2[2] = {&b, &d};
        KV *merged_arr[4];
        size_t merged_size = 0;
        WT_MERGE_SORT(unused_session, arr1, 2, arr2, 2, compare, 1, merged_arr, merged_size);
        // For duplicate key 2, the element from arr1 should be skipped.
        REQUIRE(merged_size == 3);
        REQUIRE(merged_arr[0]->key == 2);
        REQUIRE(merged_arr[0]->value == 20);
        REQUIRE(merged_arr[1]->key == 3);
        REQUIRE(merged_arr[1]->value == 30);
        REQUIRE(merged_arr[2]->key == 4);
        REQUIRE(merged_arr[2]->value == 40);
    }

    SECTION("Test with duplicates prefer_latest false")
    {
        KV a{2, 10}, c{3, 30};
        KV b{2, 20}, d{4, 40};
        KV *arr1[2] = {&a, &c};
        KV *arr2[2] = {&b, &d};
        KV *merged_arr[4];
        size_t merged_size = 0;
        WT_MERGE_SORT(unused_session, arr1, 2, arr2, 2, compare, 0, merged_arr, merged_size);
        // Both duplicate entries should be kept.
        REQUIRE(merged_size == 4);
        REQUIRE(merged_arr[0]->key == 2);
        REQUIRE(merged_arr[0]->value == 20);
        REQUIRE(merged_arr[1]->key == 2);
        REQUIRE(merged_arr[1]->value == 10);
        REQUIRE(merged_arr[2]->key == 3);
        REQUIRE(merged_arr[2]->value == 30);
        REQUIRE(merged_arr[3]->key == 4);
        REQUIRE(merged_arr[3]->value == 40);
    }

    SECTION("Test single element arrays")
    {
        KV a{5, 50};
        KV b{3, 30};
        KV *arr1[1] = {&a};
        KV *arr2[1] = {&b};
        KV *merged_arr[2];
        size_t merged_size = 0;
        WT_MERGE_SORT(unused_session, arr1, 1, arr2, 1, compare, 1, merged_arr, merged_size);
        REQUIRE(merged_size == 2);
        REQUIRE(merged_arr[0]->key == 3);
        REQUIRE(merged_arr[1]->key == 5);
    }

    SECTION("Test all identical prefer_latest true")
    {
        KV a{2, 100}, b{2, 101}, c{2, 102};
        KV d{2, 200}, e{2, 201}, f{2, 202};
        KV *arr1[3] = {&a, &b, &c};
        KV *arr2[3] = {&d, &e, &f};
        KV *merged_arr[6];
        size_t merged_size = 0;
        WT_MERGE_SORT(unused_session, arr1, 3, arr2, 3, compare, 1, merged_arr, merged_size);
        // With prefer_latest true, only the elements from arr2 are kept.
        REQUIRE(merged_size == 3);
        REQUIRE(merged_arr[0]->value == 200);
        REQUIRE(merged_arr[1]->value == 201);
        REQUIRE(merged_arr[2]->value == 202);
    }

    SECTION("Test all identical prefer_latest false")
    {
        KV a{2, 100}, b{2, 101}, c{2, 102};
        KV d{2, 200}, e{2, 201}, f{2, 202};
        KV *arr1[3] = {&a, &b, &c};
        KV *arr2[3] = {&d, &e, &f};
        KV *merged_arr[6];
        size_t merged_size = 0;
        WT_MERGE_SORT(unused_session, arr1, 3, arr2, 3, compare, 0, merged_arr, merged_size);
        // With prefer_latest false, all entries should be present.
        REQUIRE(merged_size == 6);
        REQUIRE(merged_arr[0]->value == 200);
        REQUIRE(merged_arr[1]->value == 201);
        REQUIRE(merged_arr[2]->value == 202);
        REQUIRE(merged_arr[3]->value == 100);
        REQUIRE(merged_arr[4]->value == 101);
        REQUIRE(merged_arr[5]->value == 102);
    }
    SECTION("Test with two sorted arrays")
    {
        KV arr1[9] = {{1, 101}, {3, 103}, {5, 105}, {7, 107}, {9, 109}, {11, 111}, {13, 113},
          {17, 117}, {19, 119}};
        KV arr2[10] = {{2, 202}, {3, 303}, {6, 206}, {7, 307}, {10, 210}, {12, 212}, {14, 214},
          {16, 216}, {18, 218}, {20, 220}};

        // Create arrays of pointers.
        KV *p_arr1[9], *p_arr2[10];
        for (int i = 0; i < 9; i++) {
            p_arr1[i] = &arr1[i];
        }
        for (int i = 0; i < 10; i++) {
            p_arr2[i] = &arr2[i];
        }

        // max possible size 19.
        KV *merged_arr[19];
        size_t merged_size = 0;
        WT_MERGE_SORT(unused_session, p_arr1, 9, p_arr2, 10, compare, 1, merged_arr, merged_size);

        int expected_keys[17] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20};
        int expected_values[17] = {
          101, 202, 303, 105, 206, 307, 109, 210, 111, 212, 113, 214, 216, 117, 218, 119, 220};

        REQUIRE(merged_size == 17);
        for (size_t i = 0; i < merged_size; i++) {
            REQUIRE(merged_arr[i]->key == expected_keys[i]);
            REQUIRE(merged_arr[i]->value == expected_values[i]);
        }
    }
}
