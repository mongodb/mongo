/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/makeobj_enums.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe::value {
/**
 * MakeObjSpec is a wrapper around a FieldBehavior value, a list of fields, a list of projected
 * fields, and a StringMap object.
 */
struct MakeObjSpec {
    using FieldBehavior = MakeObjFieldBehavior;

    MakeObjSpec(FieldBehavior fieldBehavior,
                std::vector<std::string> fields,
                std::vector<std::string> projects)
        : fieldBehavior(fieldBehavior),
          fields(std::move(fields)),
          projects(std::move(projects)),
          allFieldsMap(buildAllFieldsMap()),
          bloomFilter(buildBloomFilter()) {}

    MakeObjSpec(const MakeObjSpec& other)
        : fieldBehavior(other.fieldBehavior),
          fields(other.fields),
          projects(other.projects),
          allFieldsMap(buildAllFieldsMap()),
          bloomFilter(buildBloomFilter()) {}

    MakeObjSpec(MakeObjSpec&& other)
        : fieldBehavior(other.fieldBehavior),
          fields(std::move(other.fields)),
          projects(std::move(other.projects)),
          allFieldsMap(buildAllFieldsMap()),
          bloomFilter(buildBloomFilter()) {}

    StringDataMap<size_t> buildAllFieldsMap() const;

    std::array<uint8_t, 128> buildBloomFilter() const;

    size_t getApproximateSize() const;

    std::string toString() const;

    inline static size_t getLowestNBits(size_t val, size_t n) {
        return val & ((1u << n) - 1u);
    }

    // This function assumes that 'length' is not 0.
    inline static size_t computeBloomIdx1(const char* name, size_t length) {
        // The lowest 5 bits of 'name[length - 1]' and the lowest 2 bits of 'length' are good
        // sources of entropy. Combine them to generate a pseudo-random index between 0 and 127
        // inclusive.
        return getLowestNBits(size_t(name[length - 1]) + (length << 5u), 7);
    }

    // This function assumes that 'length' is not 0.
    inline static size_t computeBloomIdx2(const char* name, size_t length, size_t bloomIdx1) {
        // The lowest 5 bits of 'name[0]' are a good source of entropy. Multiply 'name[0]' by 3
        // and add 'bloomIdx1' to generate a pseudo-random index between 0 and 127 inclusive.
        return getLowestNBits(size_t(name[0]) + (size_t(name[0]) << 1u) + bloomIdx1, 7);
    }

    const FieldBehavior fieldBehavior;
    const std::vector<std::string> fields;
    const std::vector<std::string> projects;
    const StringDataMap<size_t> allFieldsMap;
    const std::array<uint8_t, 128> bloomFilter;
};
}  // namespace mongo::sbe::value
