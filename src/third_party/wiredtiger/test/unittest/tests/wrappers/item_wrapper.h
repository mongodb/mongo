/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <string>

class item_wrapper {
public:
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
