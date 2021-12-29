/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sort_executor.h"

#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/exec/working_set.h"

namespace mongo {
namespace {
/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
std::string nextFileName() {
    static AtomicWord<unsigned> sortExecutorFileCounter;
    return "extsort-sort-executor." + std::to_string(sortExecutorFileCounter.fetchAndAdd(1));
}
}  // namespace
}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"

MONGO_CREATE_SORTER(mongo::Value,
                    mongo::Document,
                    mongo::SortExecutor<mongo::Document>::Comparator);
MONGO_CREATE_SORTER(mongo::Value,
                    mongo::SortableWorkingSetMember,
                    mongo::SortExecutor<mongo::SortableWorkingSetMember>::Comparator);
MONGO_CREATE_SORTER(mongo::Value, mongo::BSONObj, mongo::SortExecutor<mongo::BSONObj>::Comparator);
