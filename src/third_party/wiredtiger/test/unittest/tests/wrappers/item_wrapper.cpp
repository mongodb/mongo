/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wiredtiger.h"
#include "item_wrapper.h"

item_wrapper::item_wrapper(const char *string) : _string(string)
{
    _item.data = _string.c_str();
    _item.size = _string.length() + 1;
    _item.mem = nullptr;
    _item.memsize = 0;
    _item.flags = 0;
}

item_wrapper::~item_wrapper()
{
    _item.data = nullptr;
    _item.size = 0;
}
