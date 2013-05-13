// btreetests.cpp : Btree unit tests
//

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/pch.h"

#include "mongo/db/btree.h"
#include "mongo/db/btreecursor.h"
#include "mongo/db/db.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"

#define BtreeBucket BtreeBucket<V0>
#define btree btree<V0>
#define btreemod btreemod<V0>
#define testName "btree"
#define BTVERSION 0
namespace BtreeTests0 {
#include "mongo/dbtests/btreetests.inl"
}

#undef BtreeBucket
#undef btree
#undef btreemod
#define BtreeBucket BtreeBucket<V1>
#define btree btree<V1>
#define btreemod btreemod<V1>
#undef testName
#define testName "btree1"
#undef BTVERSION
#define BTVERSION 1
namespace BtreeTests1 {
#include "mongo/dbtests/btreetests.inl"
}
