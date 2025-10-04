/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"

#include <span>

namespace mongo::sbe::value {
class KeyStringEntry {
public:
    KeyStringEntry() = default;

    explicit KeyStringEntry(const SortedDataKeyValueView& view);

    explicit KeyStringEntry(const key_string::Value& value);

    KeyStringEntry& operator=(KeyStringEntry&& other) noexcept;

    bool owned() const {
        return _value != boost::none;
    }

    std::span<const char> getKeyStringView() const {
        return _key;
    }

    std::span<const char> getTypeBitsView() const {
        return _typeBits;
    }

    std::span<const char> getRecordIdView() const {
        return _rid;
    }

    key_string::Version getVersion() const {
        return _version;
    }

    int getSerializedSize() const {
        return sizeof(_version) + _key.size() + _typeBits.size();
    }

    int compare(const KeyStringEntry& other) const {
        return key_string::compare(_key, other._key);
    }

    std::size_t hash() const {
        return absl::Hash<absl::string_view>{}(absl::string_view{_key.data(), _key.size()});
    }

    const key_string::Value& getValue() const {
        tassert(8843701, "value is not available", _value);
        return *_value;
    }

    key_string::Value getValueCopy() const {
        return key_string::Value::makeValue(_version, _key, _rid, _typeBits);
    }

    std::unique_ptr<KeyStringEntry> makeCopy() const;

    // Serializes key string (without record id) into a storable format with TypeBits information.
    // The serialized format takes the following form:
    //   [version][keystring size][keystring encoding][typebits encoding]
    void serialize(BufBuilder& buf) const;

    // Deserialize the Value from a serialized format.
    static KeyStringEntry* deserialize(BufReader& buf);

    std::string toString() const {
        return hexblob::encode({_key.data(), _key.size()});
    }

private:
    std::span<const char> _key;
    std::span<const char> _typeBits;
    // For indexConsistencyCheck.
    std::span<const char> _rid;
    key_string::Version _version;
    boost::optional<key_string::Value> _value;  // none means unowned view.

    void _initFromView(const SortedDataKeyValueView& view);
};
}  // namespace mongo::sbe::value
