/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <list>
#include <catch2/catch.hpp>
#include "wt_internal.h"

template <class T> struct tailq_entry {
    explicit tailq_entry<T>(T const &value) : _value(value){};
    TAILQ_ENTRY(tailq_entry<T>) _queue;
    T _value;
};

template <class T> class TestTailQWrapper {
    public:
    TestTailQWrapper();
    ~TestTailQWrapper();
    void pushBack(T const &value);
    void pushBack(std::list<T> const &values);
    void removeValue(T const &value);
    std::list<T> copyItemsFromTailQ();

    private:
    TAILQ_HEAD(, tailq_entry<T>) _tailq;
};

template <class T> TestTailQWrapper<T>::TestTailQWrapper()
{
    _tailq = TAILQ_HEAD_INITIALIZER(_tailq);
}

template <class T> TestTailQWrapper<T>::~TestTailQWrapper()
{
    tailq_entry<T> *item = TAILQ_FIRST(&_tailq);
    while (item) {
        TAILQ_REMOVE(&_tailq, item, _queue);
        free(item);
        item = TAILQ_FIRST(&_tailq);
    }
}

template <class T>
void
TestTailQWrapper<T>::pushBack(T const &value)
{
    auto item = static_cast<tailq_entry<T> *>(malloc(sizeof(tailq_entry<T>)));
    item->_value = value;

    TAILQ_INSERT_TAIL(&_tailq, item, _queue);
}

template <class T>
void
TestTailQWrapper<T>::pushBack(std::list<T> const &values)
{
    std::for_each(values.begin(), values.end(), [this](const int n) { this->pushBack(n); });
}

template <class T>
void
TestTailQWrapper<T>::removeValue(T const &value)
{
    tailq_entry<T> *item = TAILQ_FIRST(&_tailq);
    bool searching = true;
    while ((item != nullptr) && searching) {
        tailq_entry<T> *nextItem = TAILQ_NEXT(item, _queue);
        if (item->_value == value) {
            TAILQ_REMOVE(&_tailq, item, _queue);
            free(item);
            searching = false;
        } else {
            item = nextItem;
        }
    }
}

template <class T>
std::list<T>
TestTailQWrapper<T>::copyItemsFromTailQ()
{
    std::list<T> items;
    tailq_entry<T> *item = nullptr;

    TAILQ_FOREACH (item, &_tailq, _queue) {
        items.push_back(item->_value);
    }

    return items;
}

TEST_CASE("Test TAILQ: add to and remove from TAILQ", "[TAILQ]")
{
    std::list<int> items{10, 20, 30, 40, 50, 60, 70};

    TestTailQWrapper<int> testTailQWrapper;
    testTailQWrapper.pushBack(items);

    SECTION("Check for correct items")
    {
        auto returnedItems = testTailQWrapper.copyItemsFromTailQ();
        CHECK(returnedItems == items);
    }

    SECTION("Check item removal")
    {
        testTailQWrapper.removeValue(30);
        auto returnedItems = testTailQWrapper.copyItemsFromTailQ();
        CHECK(returnedItems == std::list<int>{10, 20, 40, 50, 60, 70});

        testTailQWrapper.removeValue(60);
        auto returnedItems2 = testTailQWrapper.copyItemsFromTailQ();
        CHECK(returnedItems2 == std::list<int>{10, 20, 40, 50, 70});

        // 99 isn't in the TAILQ, so this will have no effect.
        testTailQWrapper.removeValue(99);
        auto returnedItems3 = testTailQWrapper.copyItemsFromTailQ();
        CHECK(returnedItems3 == std::list<int>{10, 20, 40, 50, 70});
    }
}

TEST_CASE("Test TAILQ: attempted removal from empty TAILQ", "[TAILQ]")
{
    TestTailQWrapper<int> testTailQWrapper;

    // The list is empty so this will have no effect.
    testTailQWrapper.removeValue(99);
    auto returnedItems = testTailQWrapper.copyItemsFromTailQ();
    CHECK(returnedItems.empty());
}
