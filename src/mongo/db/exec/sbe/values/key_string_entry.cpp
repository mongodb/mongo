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

#include "mongo/db/exec/sbe/values/key_string_entry.h"

namespace mongo::sbe::value {

KeyStringEntry::KeyStringEntry(const SortedDataKeyValueView& view)
    : _key(view.getKeyStringWithoutRecordIdView()),
      _typeBits(view.getTypeBitsView()),
      _rid(view.getRecordIdView()),
      _version(view.getVersion()) {}

KeyStringEntry::KeyStringEntry(std::unique_ptr<key_string::Value> value)
    : _value(std::move(value)) {
    _key = {_value->getBuffer(), _value->getSize()};
    _typeBits = _value->getTypeBitsView();
    _rid = {};
    _version = _value->getVersion();
}

KeyStringEntry& KeyStringEntry::operator=(KeyStringEntry&& other) noexcept {
    _key = other._key;
    _rid = other._rid;
    _typeBits = other._typeBits;
    _version = other._version;
    _value = std::move(other._value);
    return *this;
}

std::unique_ptr<KeyStringEntry> KeyStringEntry::makeCopy() const {
    auto newKeyString = std::make_unique<KeyStringEntry>();
    newKeyString->_value = key_string::Value::makeValue(_version, _key, _rid, _typeBits);
    // The RecordId will be appended to the key_string::Value buffer, use current key size to
    // for the new key.
    newKeyString->_key = {newKeyString->_value->getBuffer(), _key.size()};
    newKeyString->_rid = {newKeyString->_value->getBuffer() + _key.size(), _rid.size()};
    newKeyString->_typeBits = newKeyString->_value->getTypeBitsView();
    return newKeyString;
}

void KeyStringEntry::serialize(BufBuilder& buf) const {
    buf.appendUChar(static_cast<uint8_t>(_version));
    buf.appendNum(_key.size());
    buf.appendStr(_key, false);
    buf.appendNum(_typeBits.size());
    buf.appendStr(_typeBits, false);
    buf.appendNum(_rid.size());
    buf.appendStr(_rid, false);
}

KeyStringEntry* KeyStringEntry::deserialize(BufReader& buf) {
    auto version = static_cast<key_string::Version>(buf.read<uint8_t>());
    auto key = buf.readBytes(buf.read<StringData::size_type>());
    auto typeBits = buf.readBytes(buf.read<StringData::size_type>());
    auto rid = buf.readBytes(buf.read<StringData::size_type>());
    return new KeyStringEntry{key_string::Value::makeValue(version, key, rid, typeBits)};
}
}  // namespace mongo::sbe::value
