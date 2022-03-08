/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <memory>

#include <catch2/catch.hpp>

#include "wt_internal.h"

struct ExtentWrapper {
    ExtentWrapper(WT_EXT *raw) : _raw(raw) {}

    ~ExtentWrapper()
    {
        free(_raw);
    }

    WT_EXT *_raw;
};

struct ExtentListWrapper {
    /*
     * Unfortunately, some functions need raw 2D pointers, but that's not compatible with automatic
     * memory management. _list is only for allocation bookkeeping - _raw_list can be rearranged
     * however.
     */
    std::vector<std::unique_ptr<ExtentWrapper>> _list;
    std::vector<WT_EXT *> _raw_list;
};

struct SizeWrapper {
    SizeWrapper(WT_SIZE *raw) : _raw(raw) {}

    ~SizeWrapper()
    {
        free(_raw);
    }

    WT_SIZE *_raw;
};

struct SizeListWrapper {
    /*
     * Unfortunately, some functions need raw 2D pointers, but that's not compatible with automatic
     * memory management. _list is only for allocation bookkeeping - _raw_list can be rearranged
     * however since it doesn't own any data.
     */
    std::vector<std::unique_ptr<SizeWrapper>> _list;
    std::vector<WT_SIZE *> _raw_list;
};

std::unique_ptr<ExtentWrapper>
create_new_ext()
{
    /*
     * Manually alloc enough extra space for the zero-length array to encode two skip lists.
     */
    auto sz = sizeof(WT_EXT) + 2 * WT_SKIP_MAXDEPTH * sizeof(WT_EXT *);

    auto raw = (WT_EXT *)malloc(sz);
    memset(raw, 0, sz);

    return std::make_unique<ExtentWrapper>(raw);
}

std::unique_ptr<SizeWrapper>
create_new_sz()
{
    auto raw = (WT_SIZE *)malloc(sizeof(WT_SIZE));
    memset(raw, 0, sizeof(WT_SIZE));

    return std::make_unique<SizeWrapper>(raw);
}

void
print_list(WT_EXT **head)
{
    WT_EXT *extp;
    int i;

    if (head == nullptr)
        return;

    for (i = 0; i < WT_SKIP_MAXDEPTH; i++) {
        printf("L%d: ", i);

        extp = head[i];
        while (extp != nullptr) {
            printf("%p -> ", extp);
            extp = extp->next[i];
        }

        printf("X\n");
    }
}

/*
 * Creates a sane-looking "default" extent list suitable for testing:
 * L0: 1 -> 2 -> 3 -> X
 * L1: 2 -> 3 -> X
 * L2: 3 -> X
 * L3: X
 * ...
 * L9: X
 */
void
create_default_test_extent_list(ExtentListWrapper &wrapper)
{
    auto &head = wrapper._list;
    for (int i = 0; i < 3; i++)
        head.push_back(create_new_ext());

    auto first = head[0]->_raw;
    auto second = head[1]->_raw;
    auto third = head[2]->_raw;
    first->next[0] = second;
    first->next[1] = third;
    second->next[0] = third;

    auto &raw = wrapper._raw_list;
    raw.push_back(first);
    raw.push_back(second);
    raw.push_back(third);
    for (int i = 3; i < WT_SKIP_MAXDEPTH; i++)
        raw.push_back(nullptr);
}

// As above, but for a size list.
void
create_default_test_size_list(SizeListWrapper &wrapper)
{
    auto &head = wrapper._list;
    for (int i = 0; i < 3; i++)
        head.push_back(create_new_sz());

    auto first = head[0]->_raw;
    auto second = head[1]->_raw;
    auto third = head[2]->_raw;
    first->next[0] = second;
    first->next[1] = third;
    second->next[0] = third;

    auto &raw = wrapper._raw_list;
    raw.push_back(first);
    raw.push_back(second);
    raw.push_back(third);
    for (int i = 3; i < WT_SKIP_MAXDEPTH; i++)
        raw.push_back(nullptr);
}

TEST_CASE("Extent Lists: block_off_srch_last", "[extent_list]")
{
    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("empty list has empty final element")
    {
        std::vector<WT_EXT *> head(WT_SKIP_MAXDEPTH, nullptr);

        REQUIRE(__ut_block_off_srch_last(&head[0], &stack[0]) == nullptr);

        for (int i = 0; i < WT_SKIP_MAXDEPTH; i++) {
            REQUIRE(stack[i] == &head[i]);
        }
    }

    SECTION("list with one element has non-empty final element")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        auto first = create_new_ext();
        head.push_back(first->_raw);
        for (int i = 1; i < WT_SKIP_MAXDEPTH; i++)
            head.push_back(nullptr);

        REQUIRE(__ut_block_off_srch_last(&head[0], &stack[0]) == head[0]);
    }

    SECTION("list with identical skip entries returns identical stack entries")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        auto first = create_new_ext();
        for (int i = 0; i < WT_SKIP_MAXDEPTH; i++)
            head.push_back(first->_raw);

        WT_IGNORE_RET(__ut_block_off_srch_last(&head[0], &stack[0]));

        for (int i = 0; i < WT_SKIP_MAXDEPTH; i++) {
            REQUIRE(stack[i] == &head[i]->next[i]);
        }
    }

    SECTION("list with differing skip entries returns differing stack entries")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        create_default_test_extent_list(wrapper);

        WT_IGNORE_RET(__ut_block_off_srch_last(&head[0], &stack[0]));

        REQUIRE(stack[0] == &head[2]->next[0]);
        REQUIRE(stack[1] == &head[2]->next[1]);
        REQUIRE(stack[2] == &head[2]->next[2]);
        for (int i = 3; i < WT_SKIP_MAXDEPTH; i++) {
            REQUIRE(stack[i] == &head[i]);
        }
    }

    SECTION("list with differing skip entries returns final entry")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        auto first = create_new_ext();
        auto second = create_new_ext();
        first->_raw->next[0] = second->_raw;

        head.push_back(first->_raw);
        for (int i = 1; i < WT_SKIP_MAXDEPTH; i++)
            head.push_back(second->_raw);

        REQUIRE(__ut_block_off_srch_last(&head[0], &stack[0]) == second->_raw);
    }
}

TEST_CASE("Extent Lists: block_off_srch", "[extent_list]")
{
    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("can't find offset in empty list")
    {
        std::vector<WT_EXT *> head(WT_SKIP_MAXDEPTH, nullptr);

        __ut_block_off_srch(&head[0], 0, &stack[0], false);

        for (int i = 0; i < WT_SKIP_MAXDEPTH; i++)
            REQUIRE(stack[i] == &head[i]);
    }

    SECTION("exact offset match returns matching list element")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        create_default_test_extent_list(wrapper);

        head[0]->off = 1;
        head[1]->off = 2;
        head[2]->off = 3;

        __ut_block_off_srch(&head[0], 2, &stack[0], false);

        /*
         * For each level of the extent list, if the searched-for element was visible, we should
         * point to it. otherwise, we should point to the next-largest item.
         */
        REQUIRE((*stack[0])->off == 2);
        REQUIRE((*stack[1])->off == 2);
        REQUIRE((*stack[2])->off == 3);
    }

    SECTION("search for item larger than maximum in list returns end of list")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        create_default_test_extent_list(wrapper);

        head[0]->off = 1;
        head[1]->off = 2;
        head[2]->off = 3;

        __ut_block_off_srch(&head[0], 4, &stack[0], false);

        REQUIRE(stack[0] == &head[2]->next[0]);
        REQUIRE(stack[1] == &head[2]->next[1]);
        REQUIRE(stack[2] == &head[2]->next[2]);
        for (int i = 3; i < WT_SKIP_MAXDEPTH; i++)
            REQUIRE(stack[i] == &head[i]);
    }

    SECTION("respect skip offset")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        create_default_test_extent_list(wrapper);

        head[0]->next[0] = nullptr;
        head[1]->next[1] = nullptr;
        head[2]->next[0] = nullptr;

        const int depth = 10;
        head[0]->next[0 + depth] = head[1];
        head[1]->next[1 + depth] = head[2];
        head[2]->next[0 + depth] = head[2];

        head[0]->off = 1;
        head[0]->depth = depth;
        head[1]->off = 2;
        head[1]->depth = depth;
        head[2]->off = 3;
        head[2]->depth = depth;

        __ut_block_off_srch(&head[0], 2, &stack[0], true);

        // For each level of the extent list, if the searched-for element was
        // visible, we should point to it. otherwise, we should point to the
        // next-largest item.
        REQUIRE((*stack[0])->off == 2);
        REQUIRE((*stack[1])->off == 2);
        REQUIRE((*stack[2])->off == 3);
    }
}

TEST_CASE("Extent Lists: block_first_srch", "[extent_list]")
{
    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    /*
     * Note that we're not checking stack here, since __block_first_srch delegates most of its work
     * to __block_off_srch, which we're testing elsewhere.
     */

    SECTION("empty list doesn't yield a chunk")
    {
        std::vector<WT_EXT *> head(WT_SKIP_MAXDEPTH, nullptr);

        REQUIRE(__ut_block_first_srch(&head[0], 0, &stack[0]) == false);
    }

    SECTION("list with too-small chunks doesn't yield a larger chunk")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        create_default_test_extent_list(wrapper);

        head[0]->size = 1;
        head[1]->size = 2;
        head[2]->size = 3;

        REQUIRE(__ut_block_first_srch(&head[0], 4, &stack[0]) == false);
    }

    SECTION("find an appropriate chunk")
    {
        auto wrapper = ExtentListWrapper();
        auto &head = wrapper._raw_list;

        create_default_test_extent_list(wrapper);

        head[0]->size = 10;
        head[1]->size = 20;
        head[2]->size = 30;

        REQUIRE(__ut_block_first_srch(&head[0], 4, &stack[0]) == true);
    }
}

TEST_CASE("Extent Lists: block_size_srch", "[extent_list]")
{
    std::vector<WT_SIZE **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("empty size list yields first elements")
    {
        std::vector<WT_SIZE *> head(WT_SKIP_MAXDEPTH, nullptr);

        __ut_block_size_srch(&head[0], 0, &stack[0]);

        for (int i = 0; i < WT_SKIP_MAXDEPTH; i++)
            REQUIRE(stack[i] == &head[i]);
    }

    SECTION("exact size match returns matching list element")
    {
        auto wrapper = SizeListWrapper();
        auto &head = wrapper._raw_list;

        create_default_test_size_list(wrapper);

        head[0]->size = 1;
        head[1]->size = 2;
        head[2]->size = 3;

        __ut_block_size_srch(&head[0], 2, &stack[0]);

        /*
         * For each level of the extent list, if the searched-for element was visible, we should
         * point to it. otherwise, we should point to the next-largest item.
         */
        REQUIRE((*stack[0])->size == 2);
        REQUIRE((*stack[1])->size == 2);
        REQUIRE((*stack[2])->size == 3);
    }

    SECTION("search for item larger than maximum in list returns end of list")
    {
        auto wrapper = SizeListWrapper();
        auto &head = wrapper._raw_list;

        create_default_test_size_list(wrapper);

        head[0]->size = 1;
        head[1]->size = 2;
        head[2]->size = 3;

        __ut_block_size_srch(&head[0], 4, &stack[0]);

        REQUIRE(stack[0] == &head[2]->next[0]);
        REQUIRE(stack[1] == &head[2]->next[1]);
        REQUIRE(stack[2] == &head[2]->next[2]);
        for (int i = 3; i < WT_SKIP_MAXDEPTH; i++)
            REQUIRE(stack[i] == &head[i]);
    }
}
