/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/platform/atomic_word.h"

namespace mongo {

// If set to true on mongos, all aggregations delivered to the mongos which require a merging shard
// will select the primary shard as the merger. False by default, which means that the merging shard
// will be selected randomly amongst the shards participating in the query. Pipelines capable of
// merging on mongoS are unaffected by this setting, unless internalQueryProhibitMergingOnMongoS is
// true.
extern AtomicBool internalQueryAlwaysMergeOnPrimaryShard;

// If set to true on mongos, all aggregations which could otherwise merge on the mongos will be
// obliged to merge on a shard instead. Pipelines which are redirected to the shards will obey the
// value of internalQueryAlwaysMergeOnPrimaryShard. False by default, meaning that pipelines capable
// of merging on mongoS will always do so.
extern AtomicBool internalQueryProhibitMergingOnMongoS;

}  // namespace mongo
