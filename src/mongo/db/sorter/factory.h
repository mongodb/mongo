/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/sorter/limit_one_sorter.h"
#include "mongo/db/sorter/no_limit_sorter.h"
#include "mongo/db/sorter/top_k_sorter.h"

namespace mongo::sorter {
template <typename Key, typename Value>
std::unique_ptr<Sorter<Key, Value>> make(
    StringData name,
    const Options& options,
    const typename Sorter<Key, Value>::CompFn& comp,
    const typename Sorter<Key, Value>::Settings& settings = {}) {
    switch (options.limit) {
        case 0:
            return std::make_unique<NoLimitSorter<Key, Value>>(name, options, comp, settings);
        case 1:
            return std::make_unique<LimitOneSorter<Key, Value>>(comp);
        default:
            return std::make_unique<TopKSorter<Key, Value>>(name, options, comp, settings);
    }
}

template <typename Key, typename Value>
std::unique_ptr<Sorter<Key, Value>> makeFromExistingRanges(
    const std::string& fileName,
    const std::vector<SorterRange>& ranges,
    const Options& options,
    const typename Sorter<Key, Value>::CompFn& comp,
    const typename Sorter<Key, Value>::Settings& settings = {}) {
    invariant(options.tempDir);
    invariant(options.limit == 0,
              str::stream() << "Creating a Sorter from existing ranges is only available with the "
                               "NoLimitSorter (limit 0), but got limit "
                            << options.limit);

    return std::make_unique<NoLimitSorter<Key, Value>>(fileName, ranges, options, comp, settings);
}
}  // namespace mongo::sorter
