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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/index/btree_key_generator.h"

namespace mongo::sbe::value {
/**
 * SortSpec is a wrapper around a BSONObj giving a sort pattern (encoded as a BSONObj), a collator,
 * and a BtreeKeyGenerator object.
 */
class SortSpec {
public:
    SortSpec(const BSONObj& sortPattern, const CollatorInterface* collator)
        : _sortPattern(sortPattern.getOwned()), _collator(collator), _keyGen(initKeyGen()) {}

    SortSpec(const SortSpec& other)
        : _sortPattern(other._sortPattern), _collator(other._collator), _keyGen(initKeyGen()) {}

    SortSpec& operator=(const SortSpec&) = delete;

    KeyString::Value generateSortKey(const BSONObj& obj) const;

    const BSONObj& getPattern() const {
        return _sortPattern;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

private:
    BtreeKeyGenerator initKeyGen() const;

    const BSONObj _sortPattern;
    const CollatorInterface* const _collator;
    const BtreeKeyGenerator _keyGen;
};
}  // namespace mongo::sbe::value
