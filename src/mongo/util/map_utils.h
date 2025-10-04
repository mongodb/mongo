/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <utility>

namespace mongo {

/*
 * "moves" keys and values out of the map and passes them into the consumer function to own. This
 * can be used to avoid copies if the key and value in the map have heap allocations.
 */
template <class MapType, class ConsumerType>
static void extractFromMap(MapType map, const ConsumerType& consumer) {
    while (!map.empty()) {
        auto entry = map.extract(map.begin());
        consumer(std::move(entry.key()), std::move(entry.mapped()));
    }
}

/*
 * Similar to `extractFromMap` but for sets.
 */
template <class SetType, class ConsumerType>
static void extractFromSet(SetType set, const ConsumerType& consumer) {
    while (!set.empty()) {
        auto entry = set.extract(set.begin());
        consumer(std::move(entry.value()));
    }
}
}  // namespace mongo
