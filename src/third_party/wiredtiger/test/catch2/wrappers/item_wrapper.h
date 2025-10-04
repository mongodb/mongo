/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <string>

#include "wiredtiger.h"

/*
 * item_wrapper is a simple wrapper class that wraps a WT_ITEM holding a constant, read-only string.
 * It copies the string into a std::string attribute and then sets the WT_ITEM attribute to point to
 * that string.
 *
 * Its purpose is to simplify construction and memory management/cleanup for WT_ITEMs containing
 * read-only strings
 *
 * Note: The behavior is undefined if the string contained in the wrapped WT_ITEM is modified.
 */
class item_wrapper {
public:
    explicit item_wrapper(std::string const &string);
    explicit item_wrapper(const char *string);
    ~item_wrapper();
    WT_ITEM *
    get_item()
    {
        return &_item;
    };

private:
    WT_ITEM _item;
    std::string _string;
};
