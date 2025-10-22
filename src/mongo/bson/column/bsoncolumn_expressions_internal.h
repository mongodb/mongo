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

#include "mongo/base/compare_numbers.h"
#include "mongo/bson/column/bsoncolumn.h"

namespace mongo::bsoncolumn::bsoncolumn_internal {

/*
 * Collector type that performs comparisons of all appended values. Working value is replaced when
 * compare function returns true. Final value is accessed using value().
 */
template <class CMaterializer, class Compare>
requires Materializer<CMaterializer>
class CompareCollector {
    using Element = typename CMaterializer::Element;

public:
    CompareCollector(boost::intrusive_ptr<BSONElementStorage> allocator,
                     const StringDataComparator* comparator)
        : _allocator(std::move(allocator)), _comparator(comparator) {}

    static constexpr bool kCollectsPositionInfo = false;

    void eof() {
        // End of binary, store our working value.
        _storeValue();
    }

    // Append functions, we know our type and can expensive 3way generic comparisons.
    void append(bool val) {
        if (Compare{}(val, CMaterializer::template get<bool>(_working))) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(int32_t val) {
        if (Compare{}(val, CMaterializer::template get<int32_t>(_working))) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(int64_t val) {
        if (Compare{}(val, CMaterializer::template get<int64_t>(_working))) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(Decimal128 val) {
        // TODO SERVER-90961: Do not use 3way compare
        if (Compare{}(compareDecimals(val, CMaterializer::template get<Decimal128>(_working)), 0)) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(double val) {
        // TODO SERVER-90961: Do not use 3way compare
        if (Compare{}(compareDoubles(val, CMaterializer::template get<double>(_working)), 0)) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(Timestamp val) {
        if (Compare{}(val, CMaterializer::template get<Timestamp>(_working))) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(Date_t val) {
        if (Compare{}(val, CMaterializer::template get<Date_t>(_working))) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(OID val) {
        if (Compare{}(memcmp(val.view().view(),
                             CMaterializer::template get<OID>(_working).view().view(),
                             OID::kOIDSize),
                      0)) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(StringData val) {
        if (_comparator) {
            if (Compare{}(
                    _comparator->compare(val, CMaterializer::template get<StringData>(_working)),
                    0)) {
                _working = CMaterializer::materialize(*_allocator, val);
            }
        } else {
            // TODO SERVER-90961: Do not use 3way compare
            if (Compare{}(_compareElementStringValues(
                              val, CMaterializer::template get<StringData>(_working)),
                          0)) {
                _working = CMaterializer::materialize(*_allocator, val);
            }
        }
    }

    void append(const BSONBinData& val) {
        BSONBinData min = CMaterializer::template get<BSONBinData>(_working);
        if (val.length != min.length) {
            if (Compare{}(val.length, min.length)) {
                _working = CMaterializer::materialize(*_allocator, val);
            }
            return;
        }

        if (val.type != min.type) {
            if (Compare{}(val.type, min.type)) {
                _working = CMaterializer::materialize(*_allocator, val);
            }
            return;
        }

        // Include type byte in comparison
        if (Compare{}(memcmp((const std::byte*)val.data, (const std::byte*)min.data, val.length),
                      0)) {
            _working = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(const BSONCode& val) {
        if (_comparator) {
            if (Compare{}(_comparator->compare(
                              val.code, CMaterializer::template get<BSONCode>(_working).code),
                          0)) {
                _working = CMaterializer::materialize(*_allocator, val);
            }
        } else {
            // TODO SERVER-90961: Do not use 3way compare
            if (Compare{}(_compareElementStringValues(
                              val.code, CMaterializer::template get<BSONCode>(_working).code),
                          0)) {
                _working = CMaterializer::materialize(*_allocator, val);
            }
        }
    }

    // Append of element, this is called when we encounter an uncompressed literal and may result in
    // a type change.
    template <typename T>
    void append(const BSONElement& val) {
        // This if-else block handles when there were no type change.
        if constexpr (std::is_same_v<T, double>) {
            if (_type == BSONType::numberDouble) {
                append(BSONElementValue(val.value()).Double());
                return;
            }
        } else if constexpr (std::is_same_v<T, StringData>) {
            if (_type == BSONType::string) {
                append(BSONElementValue(val.value()).String());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONObj>) {
            if (_type == BSONType::object) {
                append(BSONElementValue(val.value()).Obj());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONArray>) {
            if (_type == BSONType::array) {
                append(BSONElementValue(val.value()).Array());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONBinData>) {
            if (_type == BSONType::binData) {
                append(BSONElementValue(val.value()).BinData());
                return;
            }
        } else if constexpr (std::is_same_v<T, OID>) {
            if (_type == BSONType::oid) {
                append(BSONElementValue(val.value()).ObjectID());
                return;
            }
        } else if constexpr (std::is_same_v<T, bool>) {
            if (_type == BSONType::boolean) {
                append(BSONElementValue(val.value()).Boolean());
                return;
            }
        } else if constexpr (std::is_same_v<T, Date_t>) {
            if (_type == BSONType::date) {
                append(BSONElementValue(val.value()).Date());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONRegEx>) {
            if (_type == BSONType::regEx) {
                append(BSONElementValue(val.value()).Regex());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONDBRef>) {
            if (_type == BSONType::dbRef) {
                append(BSONElementValue(val.value()).DBRef());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONCode>) {
            if (_type == BSONType::code) {
                append(BSONElementValue(val.value()).Code());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONSymbol>) {
            if (_type == BSONType::symbol) {
                append(BSONElementValue(val.value()).Symbol());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONCodeWScope>) {
            if (_type == BSONType::codeWScope) {
                append(BSONElementValue(val.value()).CodeWScope());
                return;
            }
        } else if constexpr (std::is_same_v<T, int32_t>) {
            if (_type == BSONType::numberInt) {
                append(BSONElementValue(val.value()).Int32());
                return;
            }
        } else if constexpr (std::is_same_v<T, Timestamp>) {
            if (_type == BSONType::timestamp) {
                append(BSONElementValue(val.value()).timestamp());
                return;
            }
        } else if constexpr (std::is_same_v<T, int64_t>) {
            if (_type == BSONType::numberLong) {
                append((int64_t)BSONElementValue(val.value()).Int64());
                return;
            }
        } else if constexpr (std::is_same_v<T, Decimal128>) {
            if (_type == BSONType::numberDecimal) {
                append(BSONElementValue(val.value()).Decimal());
                return;
            }
        }

        _working = CMaterializer::template materialize<T>(*_allocator, val);
        _type = val.type();
        _storeValue();
    }

    void appendPreallocated(const BSONElement& val) {
        _working = CMaterializer::materializePreallocated(val);
        _type = val.type();
        _storeValue();
    }

    // We do not need to keep track of missing or last as that will not affect comparison
    void appendMissing() {}
    void appendLast() {}
    bool isLastMissing() {
        return false;
    }
    template <typename T>
    void setLast(const BSONElement& val) {}

    // Position info is not supported
    void appendPositionInfo(int32_t n) {}

    BSONElementStorage& getAllocator() {
        return *_allocator;
    }

    Element value() const {
        return _value;
    }

private:
    void _storeValue() {
        if (CMaterializer::isMissing(_working))
            return;

        if (CMaterializer::isMissing(_value)) {
            _value = _working;
            return;
        }

        int appendedCanonical = CMaterializer::canonicalType(_working);
        int minCanonical = CMaterializer::canonicalType(_value);
        if (Compare{}(appendedCanonical, minCanonical)) {
            _value = _working;
        } else if (appendedCanonical == minCanonical &&
                   Compare{}(CMaterializer::compare(_working, _value, _comparator), 0)) {
            _value = _working;
        }
    }

    int _compareElementStringValues(StringData lhs, StringData rhs) {
        // we use memcmp as we allow zeros in UTF8 strings
        int common = std::min(lhs.size(), rhs.size());
        int res = memcmp(lhs.data(), rhs.data(), common);
        if (res)
            return res;
        // longer string is the greater one
        return lhs.size() - rhs.size();
    }

    boost::intrusive_ptr<BSONElementStorage> _allocator;
    Element _value = CMaterializer::materializeMissing(*_allocator);
    Element _working = _value;
    BSONType _type = BSONType::eoo;
    const StringDataComparator* _comparator;
};

/*
 * Collector type that calculates min and max in a single pass over the BSONColumn binary.
 */
template <class CMaterializer>
requires Materializer<CMaterializer>
class MinMaxCollector {
    using Element = typename CMaterializer::Element;

    template <typename T>
    using MinCompare = std::less<T>;

    template <typename T>
    using MaxCompare = std::greater<T>;

public:
    MinMaxCollector(boost::intrusive_ptr<BSONElementStorage> allocator,
                    const StringDataComparator* comparator)
        : _allocator(std::move(allocator)), _comparator(comparator) {}

    static constexpr bool kCollectsPositionInfo = false;

    void eof() {
        store();
    }

    void append(bool val) {
        if (MinCompare<bool>{}(val, CMaterializer::template get<bool>(_minForType))) {
            _minForType = CMaterializer::materialize(*_allocator, val);
        }
        if (MaxCompare<bool>{}(val, CMaterializer::template get<bool>(_maxForType))) {
            _maxForType = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(int32_t val) {
        if (MinCompare<int32_t>{}(val, CMaterializer::template get<int32_t>(_minForType))) {
            _minForType = CMaterializer::materialize(*_allocator, val);
        }
        if (MaxCompare<int32_t>{}(val, CMaterializer::template get<int32_t>(_maxForType))) {
            _maxForType = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(int64_t val) {
        if (MinCompare<int64_t>{}(val, CMaterializer::template get<int64_t>(_minForType))) {
            _minForType = CMaterializer::materialize(*_allocator, val);
        }
        if (MaxCompare<int64_t>{}(val, CMaterializer::template get<int64_t>(_maxForType))) {
            _maxForType = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(Decimal128 val) {
        // TODO SERVER-90961: Do not use 3way compare
        if (MinCompare<int>{}(
                compareDecimals(val, CMaterializer::template get<Decimal128>(_minForType)), 0)) {
            _minForType = CMaterializer::materialize(*_allocator, val);
        }
        if (MaxCompare<int>{}(
                compareDecimals(val, CMaterializer::template get<Decimal128>(_maxForType)), 0)) {
            _maxForType = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(double val) {
        // TODO SERVER-90961: Do not use 3way compare
        if (MinCompare<int>{}(compareDoubles(val, CMaterializer::template get<double>(_minForType)),
                              0)) {
            _minForType = CMaterializer::materialize(*_allocator, val);
        }
        if (MaxCompare<int>{}(compareDoubles(val, CMaterializer::template get<double>(_maxForType)),
                              0)) {
            _maxForType = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(Timestamp val) {
        if (MinCompare<Timestamp>{}(val, CMaterializer::template get<Timestamp>(_minForType))) {
            _minForType = CMaterializer::materialize(*_allocator, val);
        }
        if (MaxCompare<Timestamp>{}(val, CMaterializer::template get<Timestamp>(_maxForType))) {
            _maxForType = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(Date_t val) {
        if (MinCompare<Date_t>{}(val, CMaterializer::template get<Date_t>(_minForType))) {
            _minForType = CMaterializer::materialize(*_allocator, val);
        }
        if (MaxCompare<Date_t>{}(val, CMaterializer::template get<Date_t>(_maxForType))) {
            _maxForType = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(OID val) {
        if (MinCompare<int>{}(memcmp(val.view().view(),
                                     CMaterializer::template get<OID>(_minForType).view().view(),
                                     OID::kOIDSize),
                              0)) {
            _minForType = CMaterializer::materialize(*_allocator, val);
        }
        if (MaxCompare<int>{}(memcmp(val.view().view(),
                                     CMaterializer::template get<OID>(_maxForType).view().view(),
                                     OID::kOIDSize),
                              0)) {
            _maxForType = CMaterializer::materialize(*_allocator, val);
        }
    }

    void append(StringData val) {
        if (_comparator) {
            if (MinCompare<int>{}(
                    _comparator->compare(val, CMaterializer::template get<StringData>(_minForType)),
                    0)) {
                _minForType = CMaterializer::materialize(*_allocator, val);
            }
            if (MaxCompare<int>{}(
                    _comparator->compare(val, CMaterializer::template get<StringData>(_maxForType)),
                    0)) {
                _maxForType = CMaterializer::materialize(*_allocator, val);
            }
        } else {
            // TODO SERVER-90961: Do not use 3way compare
            if (MinCompare<int>{}(_compareElementStringValues(
                                      val, CMaterializer::template get<StringData>(_minForType)),
                                  0)) {
                _minForType = CMaterializer::materialize(*_allocator, val);
            }
            if (MaxCompare<int>{}(_compareElementStringValues(
                                      val, CMaterializer::template get<StringData>(_maxForType)),
                                  0)) {
                _maxForType = CMaterializer::materialize(*_allocator, val);
            }
        }
    }

    void append(const BSONBinData& val) {
        [&]() {
            BSONBinData min = CMaterializer::template get<BSONBinData>(_minForType);
            if (val.length != min.length) {
                if (MinCompare<int>{}(val.length, min.length)) {
                    _minForType = CMaterializer::materialize(*_allocator, val);
                }
                return;
            }

            if (val.type != min.type) {
                if (MinCompare<BinDataType>{}(val.type, min.type)) {
                    _minForType = CMaterializer::materialize(*_allocator, val);
                }
                return;
            }

            // Include type byte in comparison
            if (MinCompare<int>{}(
                    memcmp((const std::byte*)val.data, (const std::byte*)min.data, val.length),
                    0)) {
                _minForType = CMaterializer::materialize(*_allocator, val);
            }
        }();
        [&]() {
            BSONBinData max = CMaterializer::template get<BSONBinData>(_maxForType);
            if (val.length != max.length) {
                if (MaxCompare<int>{}(val.length, max.length)) {
                    _maxForType = CMaterializer::materialize(*_allocator, val);
                }
                return;
            }

            if (val.type != max.type) {
                if (MaxCompare<BinDataType>{}(val.type, max.type)) {
                    _maxForType = CMaterializer::materialize(*_allocator, val);
                }
                return;
            }

            // Include type byte in comparison
            if (MaxCompare<int>{}(
                    memcmp((const std::byte*)val.data, (const std::byte*)max.data, val.length),
                    0)) {
                _maxForType = CMaterializer::materialize(*_allocator, val);
            }
        }();
    }

    void append(const BSONCode& val) {
        if (_comparator) {
            if (MinCompare<int>{}(
                    _comparator->compare(val.code,
                                         CMaterializer::template get<BSONCode>(_minForType).code),
                    0)) {
                _minForType = CMaterializer::materialize(*_allocator, val);
            }
            if (MaxCompare<int>{}(
                    _comparator->compare(val.code,
                                         CMaterializer::template get<BSONCode>(_maxForType).code),
                    0)) {
                _maxForType = CMaterializer::materialize(*_allocator, val);
            }
        } else {
            // TODO SERVER-90961: Do not use 3way compare
            if (MinCompare<int>{}(
                    _compareElementStringValues(
                        val.code, CMaterializer::template get<BSONCode>(_minForType).code),
                    0)) {
                _minForType = CMaterializer::materialize(*_allocator, val);
            }
            if (MaxCompare<int>{}(
                    _compareElementStringValues(
                        val.code, CMaterializer::template get<BSONCode>(_maxForType).code),
                    0)) {
                _maxForType = CMaterializer::materialize(*_allocator, val);
            }
        }
    }

    template <typename T>
    void append(const BSONElement& val) {
        if constexpr (std::is_same_v<T, double>) {
            if (_type == BSONType::numberDouble) {
                append(BSONElementValue(val.value()).Double());
                return;
            }
        } else if constexpr (std::is_same_v<T, StringData>) {
            if (_type == BSONType::string) {
                append(BSONElementValue(val.value()).String());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONObj>) {
            if (_type == BSONType::object) {
                append(BSONElementValue(val.value()).Obj());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONArray>) {
            if (_type == BSONType::array) {
                append(BSONElementValue(val.value()).Array());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONBinData>) {
            if (_type == BSONType::binData) {
                append(BSONElementValue(val.value()).BinData());
                return;
            }
        } else if constexpr (std::is_same_v<T, OID>) {
            if (_type == BSONType::oid) {
                append(BSONElementValue(val.value()).ObjectID());
                return;
            }
        } else if constexpr (std::is_same_v<T, bool>) {
            if (_type == BSONType::boolean) {
                append(BSONElementValue(val.value()).Boolean());
                return;
            }
        } else if constexpr (std::is_same_v<T, Date_t>) {
            if (_type == BSONType::date) {
                append(BSONElementValue(val.value()).Date());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONRegEx>) {
            if (_type == BSONType::regEx) {
                append(BSONElementValue(val.value()).Regex());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONDBRef>) {
            if (_type == BSONType::dbRef) {
                append(BSONElementValue(val.value()).DBRef());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONCode>) {
            if (_type == BSONType::code) {
                append(BSONElementValue(val.value()).Code());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONSymbol>) {
            if (_type == BSONType::symbol) {
                append(BSONElementValue(val.value()).Symbol());
                return;
            }
        } else if constexpr (std::is_same_v<T, BSONCodeWScope>) {
            if (_type == BSONType::codeWScope) {
                append(BSONElementValue(val.value()).CodeWScope());
                return;
            }
        } else if constexpr (std::is_same_v<T, int32_t>) {
            if (_type == BSONType::numberInt) {
                append(BSONElementValue(val.value()).Int32());
                return;
            }
        } else if constexpr (std::is_same_v<T, Timestamp>) {
            if (_type == BSONType::timestamp) {
                append(BSONElementValue(val.value()).timestamp());
                return;
            }
        } else if constexpr (std::is_same_v<T, int64_t>) {
            if (_type == BSONType::numberLong) {
                append((int64_t)BSONElementValue(val.value()).Int64());
                return;
            }
        } else if constexpr (std::is_same_v<T, Decimal128>) {
            if (_type == BSONType::numberDecimal) {
                append(BSONElementValue(val.value()).Decimal());
                return;
            }
        }

        _minForType = CMaterializer::template materialize<T>(*_allocator, val);
        _maxForType = _minForType;
        _type = val.type();
        store();
    }

    void store() {
        [&]() {
            if (CMaterializer::isMissing(_minForType))
                return;

            if (CMaterializer::isMissing(_min)) {
                _min = _minForType;
                return;
            }

            int appendedCanonical = CMaterializer::canonicalType(_minForType);
            int minCanonical = CMaterializer::canonicalType(_min);
            if (MinCompare<int>{}(appendedCanonical, minCanonical)) {
                _min = _minForType;
            } else if (appendedCanonical == minCanonical &&
                       MinCompare<int>{}(CMaterializer::compare(_minForType, _min, _comparator),
                                         0)) {
                _min = _minForType;
            }
        }();
        [&]() {
            if (CMaterializer::isMissing(_maxForType))
                return;

            if (CMaterializer::isMissing(_max)) {
                _max = _maxForType;
                return;
            }

            int appendedCanonical = CMaterializer::canonicalType(_maxForType);
            int maxCanonical = CMaterializer::canonicalType(_max);
            if (MaxCompare<int>{}(appendedCanonical, maxCanonical)) {
                _max = _maxForType;
            } else if (appendedCanonical == maxCanonical &&
                       MaxCompare<int>{}(CMaterializer::compare(_maxForType, _max, _comparator),
                                         0)) {
                _max = _maxForType;
            }
        }();
    }

    void appendPreallocated(const BSONElement& val) {
        _minForType = CMaterializer::materializePreallocated(val);
        _maxForType = _minForType;
        _type = val.type();
        store();
    }

    // Does not update _last, should not be repeated by appendLast()
    void appendMissing() {}

    // Appends last value that was not Missing
    void appendLast() {}

    bool isLastMissing() {
        return false;
    }

    // Sets the last value without appending anything. This should be called to update _last to be
    // the element in the reference object, and for missing top-level objects. Otherwise the append
    // methods will take care of updating _last as needed.
    template <typename T>
    void setLast(const BSONElement& val) {}

    void appendPositionInfo(int32_t n) {}

    BSONElementStorage& getAllocator() {
        return *_allocator;
    }

    std::pair<Element, Element> minmax() const {
        return std::make_pair(_min, _max);
    }

private:
    int _compareElementStringValues(StringData lhs, StringData rhs) {
        // we use memcmp as we allow zeros in UTF8 strings
        int common = std::min(lhs.size(), rhs.size());
        int res = memcmp(lhs.data(), rhs.data(), common);
        if (res)
            return res;
        // longer string is the greater one
        return lhs.size() - rhs.size();
    }

    boost::intrusive_ptr<BSONElementStorage> _allocator;
    Element _min = CMaterializer::materializeMissing(*_allocator);
    Element _max = _min;
    Element _minForType = _min;
    Element _maxForType = _min;
    BSONType _type = BSONType::eoo;
    const StringDataComparator* _comparator;
};

template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element first(const char* buffer,
                                      size_t size,
                                      boost::intrusive_ptr<BSONElementStorage> allocator) {
    const char* ptr = buffer;
    const char* end = buffer + size;

    while (ptr < end) {
        const uint8_t control = *ptr;
        if (control == stdx::to_underlying(BSONType::eoo)) {
            uassert(
                9095605, "BSONColumn data ended without reaching end of buffer", ptr + 1 == end);
            // We've reached end of stream without encountering a value, return missing.
            return CMaterializer{}.materializeMissing(*allocator);
        } else if (isUncompressedLiteralControlByte(control)) {
            // Uncompressed literal found, it cannot be skip so return as our first value.
            BSONElement literal(ptr, 1, BSONElement::TrustedInitTag{});
            return CMaterializer{}.template materialize<BSONElement>(*allocator, literal);
        } else if (isInterleavedStartControlByte(control)) {
            // TODO SERVER-90962: Implement optimized version of first() in the presence of
            // interleaved mode. Unoptimized implementation decompresses everything and returns
            // first non-skipped element.
            std::vector<typename CMaterializer::Element> buf;
            Collector<CMaterializer, std::vector<typename CMaterializer::Element>> collector(
                buf, allocator);
            BSONColumnBlockBased(buffer, size).decompress(collector);

            auto it = std::find_if_not(buf.begin(), buf.end(), [](const auto& elem) {
                return CMaterializer::isMissing(elem);
            });
            if (it != buf.end()) {
                return *it;
            }
        } else {
            // Simple8b blocks before uncompressed literal are defined as skip, so we do not need to
            // unpack them and can just position our pointer after this block.
            uassert(9095606,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            ptr += size + 1;
        }
    }
    return {};
}

template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element last(const char* buffer,
                                     size_t size,
                                     boost::intrusive_ptr<BSONElementStorage> allocator) {
    const char* ptr = buffer;
    const char* end = buffer + size;

    while (ptr < end) {
        const uint8_t control = *ptr;
        if (control == stdx::to_underlying(BSONType::eoo)) {
            uassert(
                9095607, "BSONColumn data ended without reaching end of buffer", ptr + 1 == end);
            // We've reached end of stream without encountering a value, return missing.
            return CMaterializer{}.materializeMissing(*allocator);
        } else if (isUncompressedLiteralControlByte(control)) {
            // Uncompressed literal found, calculate delta to last for this type then check if we
            // are at end of stream which means we can materialize our value and return.
            BSONElement literal(ptr, 1, BSONElement::TrustedInitTag{});
            ptr += literal.size();
            switch (literal.type()) {
                case BSONType::boolean: {
                    int64_t last = literal.boolean();
                    ptr = BSONColumnBlockDecompressHelpers::lastDelta(ptr, end, last);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095608,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.materialize(*allocator, static_cast<bool>(last));
                    }
                } break;
                case BSONType::numberInt: {
                    int64_t last = literal._numberInt();
                    ptr = BSONColumnBlockDecompressHelpers::lastDelta(ptr, end, last);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095609,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.materialize(*allocator, static_cast<int32_t>(last));
                    }
                } break;
                case BSONType::numberLong: {
                    int64_t last = literal._numberLong();
                    ptr = BSONColumnBlockDecompressHelpers::lastDelta<int64_t>(ptr, end, last);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095610,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.materialize(*allocator, last);
                    }
                } break;
                case BSONType::numberDecimal: {
                    int128_t last = Simple8bTypeUtil::encodeDecimal128(literal._numberDecimal());
                    ptr = BSONColumnBlockDecompressHelpers::lastDelta(ptr, end, last);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095611,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.materialize(
                            *allocator, Simple8bTypeUtil::decodeDecimal128(last));
                    }
                } break;
                case BSONType::numberDouble: {
                    double last = literal._numberDouble();
                    ptr = BSONColumnBlockDecompressHelpers::lastDouble(ptr, end, last);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095612,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.materialize(*allocator, last);
                    }
                } break;
                case BSONType::timestamp: {
                    int64_t last = literal.timestampValue();
                    ptr = BSONColumnBlockDecompressHelpers::lastDeltaOfDelta(ptr, end, last);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095613,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.materialize(*allocator,
                                                           static_cast<Timestamp>(last));
                    }
                } break;
                case BSONType::date: {
                    int64_t last = literal.date().toMillisSinceEpoch();
                    ptr = BSONColumnBlockDecompressHelpers::lastDeltaOfDelta(ptr, end, last);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095614,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.materialize(*allocator,
                                                           Date_t::fromMillisSinceEpoch(last));
                    }
                } break;
                case BSONType::oid: {
                    int64_t last = Simple8bTypeUtil::encodeObjectId(literal.__oid());
                    ptr = BSONColumnBlockDecompressHelpers::lastDeltaOfDelta(ptr, end, last);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095615,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.materialize(
                            *allocator,
                            Simple8bTypeUtil::decodeObjectId(last,
                                                             literal.__oid().getInstanceUnique()));
                    }
                } break;
                case BSONType::string: {
                    auto encoded = Simple8bTypeUtil::encodeString(literal.valueStringData());
                    int128_t last;
                    int128_t reference;
                    ptr = BSONColumnBlockDecompressHelpers::lastString(
                        ptr, end, encoded, last, reference);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095616,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        if (last == reference && (encoded.has_value() || reference == 0)) {
                            return CMaterializer{}.materializePreallocated(literal);
                        } else {
                            auto string = Simple8bTypeUtil::decodeString(last);
                            return CMaterializer{}.materialize(
                                *allocator,
                                StringData((const char*)string.str.data(), string.size));
                        }
                    }
                } break;
                case BSONType::binData: {
                    int size;
                    const char* binary = literal.binData(size);
                    if (size <= 16) {
                        int128_t last = *Simple8bTypeUtil::encodeBinary(binary, size);
                        ptr = BSONColumnBlockDecompressHelpers::lastDelta(ptr, end, last);

                        if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                            uassert(9095617,
                                    "BSONColumn data ended without reaching end of buffer",
                                    ptr + 1 == end);
                            char data[16];
                            Simple8bTypeUtil::decodeBinary(last, data, size);
                            return CMaterializer{}.materialize(
                                *allocator, BSONBinData(data, size, literal.binDataType()));
                        }

                    } else {
                        ptr = BSONColumnBlockDecompressHelpers::validateLiteral<int128_t>(ptr, end);

                        if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                            uassert(9095618,
                                    "BSONColumn data ended without reaching end of buffer",
                                    ptr + 1 == end);
                            return CMaterializer{}.materializePreallocated(literal);
                        }
                    }

                } break;
                case BSONType::code: {
                    auto encoded = Simple8bTypeUtil::encodeString(literal.valueStringData());
                    int128_t last;
                    int128_t reference;
                    ptr = BSONColumnBlockDecompressHelpers::lastString(
                        ptr, end, encoded, last, reference);
                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095619,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        if (last == reference && (encoded.has_value() || reference == 0)) {
                            return CMaterializer{}.materializePreallocated(literal);
                        } else {
                            auto string = Simple8bTypeUtil::decodeString(last);
                            return CMaterializer{}.materialize(
                                *allocator,
                                BSONCode(StringData((const char*)string.str.data(), string.size)));
                        }
                    }
                } break;
                case BSONType::object:
                case BSONType::array:
                case BSONType::undefined:
                case BSONType::null:
                case BSONType::regEx:
                case BSONType::dbRef:
                case BSONType::codeWScope:
                case BSONType::symbol:
                case BSONType::minKey:
                case BSONType::maxKey:
                    // Non-delta types, deltas should only contain skip or 0
                    ptr = BSONColumnBlockDecompressHelpers::validateLiteral<int64_t>(ptr, end);

                    if (*ptr == stdx::to_underlying(BSONType::eoo)) {
                        uassert(9095620,
                                "BSONColumn data ended without reaching end of buffer",
                                ptr + 1 == end);
                        return CMaterializer{}.template materialize<BSONElement>(*allocator,
                                                                                 literal);
                    }
                    break;
                default:
                    uasserted(9095621, "Type not implemented");
                    break;
            }
        } else if (isInterleavedStartControlByte(control)) {
            // TODO SERVER-90962: Implement optimized version of first() in the presence of
            // interleaved mode. Unoptimized implementation decompresses everything and returns
            // first non-skipped element.
            std::vector<typename CMaterializer::Element> buf;
            Collector<CMaterializer, std::vector<typename CMaterializer::Element>> collector(
                buf, allocator);
            BSONColumnBlockBased(buffer, size).decompress(collector);

            auto it = std::find_if_not(buf.rbegin(), buf.rend(), [](const auto& elem) {
                return CMaterializer::isMissing(elem);
            });
            if (it != buf.rend()) {
                return *it;
            }
        } else {
            // Simple8b blocks before uncompressed literal are defined as skip, so we do not need to
            // unpack them and can just position our pointer after this block.

            uassert(9095622,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            ptr += size + 1;
        }
    }
    return {};
}

template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element min(const char* buffer,
                                    size_t size,
                                    boost::intrusive_ptr<BSONElementStorage> allocator,
                                    const StringDataComparator* comparator) {

    CompareCollector<CMaterializer, std::less<>> collector(allocator, comparator);
    BSONColumnBlockBased(buffer, size).decompress(collector);
    return collector.value();
}

template <class CMaterializer>
requires Materializer<CMaterializer>
typename CMaterializer::Element max(const char* buffer,
                                    size_t size,
                                    boost::intrusive_ptr<BSONElementStorage> allocator,
                                    const StringDataComparator* comparator) {
    CompareCollector<CMaterializer, std::greater<>> collector(allocator, comparator);
    BSONColumnBlockBased(buffer, size).decompress(collector);
    return collector.value();
}

template <class CMaterializer>
requires Materializer<CMaterializer>
typename std::pair<typename CMaterializer::Element, typename CMaterializer::Element> minmax(
    const char* buffer,
    size_t size,
    boost::intrusive_ptr<BSONElementStorage> allocator,
    const StringDataComparator* comparator) {
    MinMaxCollector<CMaterializer> collector(allocator, comparator);
    BSONColumnBlockBased(buffer, size).decompress(collector);
    return collector.minmax();
}

}  // namespace mongo::bsoncolumn::bsoncolumn_internal
