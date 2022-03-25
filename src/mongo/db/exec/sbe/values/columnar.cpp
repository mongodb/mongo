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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/values/columnar.h"
#include "mongo/logv2/log.h"

namespace mongo::sbe {
namespace {
// While reconstructing an object, it may be necessary to leave placeholders that are
// filled in later. We use null as this placeholder. Some of the code below assumes that
// the placeholder is a shallow value.
constexpr value::TypeTags kPlaceHolderType = value::TypeTags::Null;
constexpr value::Value kPlaceHolderValue = 0;

/*
 * Array info reader based on a string representation of arrayInfo. Allows for reading/peeking of
 * individual components.
 */
class ArrInfoReader {
public:
    explicit ArrInfoReader(StringData arrInfoStr) : _arrInfo(arrInfoStr) {}

    char nextChar() const {
        if (_offsetInArrInfo == _arrInfo.size()) {
            // Reaching the end of the array info means an unlimited number of '|'s.
            return '|';
        }
        return _arrInfo[_offsetInArrInfo];
    }

    char takeNextChar() {
        if (_offsetInArrInfo == _arrInfo.size()) {
            // Reaching the end of the array info means an unlimited number of '|'s.
            return '|';
        }
        return _arrInfo[_offsetInArrInfo++];
    }

    size_t takeNumber() {
        return SplitCellView::readNumber(_arrInfo, &_offsetInArrInfo);
    }

    bool empty() const {
        return _arrInfo.empty();
    }

    /*
     * Returns whether more explicit components are yet to be consumed. Since array info logically
     * ends with an infinite stream of |, this function indicates whether there are more components
     * which are physically present to be read, not including the infinite sequence of |.
     */
    bool moreExplicitComponents() const {
        return _offsetInArrInfo < _arrInfo.size();
    }

    StringData rawArrInfo() const {
        return _arrInfo;
    }

private:
    StringData _arrInfo;
    size_t _offsetInArrInfo = 0;
};

/*
 * Tracks state necessary for reconstructing objects including the array info reader, the values
 * available for extraction, and the path.
 */
class AddToDocumentState {
public:
    AddToDocumentState(TranslatedCell& translatedCell, ArrInfoReader reader)
        : cell(translatedCell), arrInfoReader(std::move(reader)) {}

    /*
     * Returns whether we've consumed all of the values.
     */
    bool done() {
        return !(cell.moreValues() || arrInfoReader.moreExplicitComponents());
    }

    std::pair<value::TypeTags, value::Value> extractAndCopyValue() {
        auto [tag, val] = cell.nextValue();
        return value::copyValue(tag, val);
    }

    StringData restOfPath() {
        return cell.path.substr(offsetInPath);
    }

    /*
     * Advances the path pointer to the next path component and invokes the callback with the next
     * path component.  After the callback returns, it restores the AddToDocumentState offset into
     * the path.
     */
    auto withNextPathComponent(const std::function<void(StringData)>& fn) {
        invariant(offsetInPath != std::string::npos);
        auto oldOffset = offsetInPath;

        auto nextDot = cell.path.find('.', offsetInPath);
        if (nextDot == std::string::npos) {
            nextDot = cell.path.size();
        }

        StringData field = cell.path.substr(offsetInPath, nextDot - offsetInPath);
        offsetInPath = nextDot == cell.path.size() ? nextDot : nextDot + 1;
        fn(field);
        offsetInPath = oldOffset;
    }

    /*
     * Returns true when there are no path components left to be consumed by the
     * 'withNextPathComponent' function.
     */
    bool atLastPathComponent() const {
        return offsetInPath == cell.path.size();
    }

    TranslatedCell& cell;
    ArrInfoReader arrInfoReader;

private:
    size_t offsetInPath = 0;
    size_t offsetInArrInfo = 0;
};

/*
 * Helper for finding (or creating) a field of a certain type in an SBE object.
 */
template <class T, value::TypeTags Tag, class MakeT>
T* findOrAdd(StringData name, sbe::value::Object* obj, MakeT makeT) {
    auto innerTagVal = obj->getField(name);

    if (innerTagVal.first == value::TypeTags::Nothing) {
        auto [tag, val] = makeT();
        invariant(tag == Tag);
        obj->push_back(name, Tag, val);
        return value::bitcastTo<T*>(obj->getAt(obj->size() - 1).second);
    } else {
        invariant(innerTagVal.first == Tag);
        return value::bitcastTo<T*>(innerTagVal.second);
    }
}

/*
 * Look for a field 'name' that is an object, and return it. If the field
 * does not exist, it will be created with a placeholder empty object.
 */
value::Object* findOrAddObjInObj(StringData name, sbe::value::Object* obj) {
    return findOrAdd<value::Object, value::TypeTags::Object>(name, obj, value::makeNewObject);
}

/*
 * Similar to above, for arrays.
 */
value::Array* findOrAddArrInObj(StringData name, sbe::value::Object* obj) {
    return findOrAdd<value::Array, value::TypeTags::Array>(name, obj, value::makeNewArray);
}

/*
 * Helper for finding or creating an element of a certain type in an SBE Array.
 */
template <class T, value::TypeTags Tag, class MakeT>
T* findOrAddInArr(size_t idx, sbe::value::Array* arr, MakeT makeT) {
    invariant(idx < arr->size());
    auto innerTagVal = arr->getAt(idx);

    if (innerTagVal.first == kPlaceHolderType && innerTagVal.second == kPlaceHolderValue) {
        auto [innerTag, innerVal] = makeT();
        invariant(innerTag == Tag);
        arr->setAt(idx, Tag, innerVal);

        return value::bitcastTo<T*>(innerVal);
    } else {
        invariant(innerTagVal.first == Tag);
        return value::bitcastTo<T*>(innerTagVal.second);
    }
}

value::Object* findOrAddObjInArr(size_t idx, sbe::value::Array* arr) {
    return findOrAddInArr<value::Object, value::TypeTags::Object>(idx, arr, value::makeNewObject);
}
value::Array* findOrAddArrInArr(size_t idx, sbe::value::Array* arr) {
    return findOrAddInArr<value::Array, value::TypeTags::Array>(idx, arr, value::makeNewArray);
}

/*
 * Adds the given tag,val SBE value to the 'out' object, assuming that there are no arrays along
 * the remaining path stored by 'state'.
 */
void addToObjectNoArrays(value::TypeTags tag,
                         value::Value val,
                         AddToDocumentState& state,
                         value::Object& out,
                         size_t idx) {
    state.withNextPathComponent([&](StringData nextPathComponent) {
        if (state.atLastPathComponent()) {
            dassert(!out.contains(nextPathComponent));
            out.push_back(nextPathComponent, tag, val);
            return;
        }

        auto* innerObj = findOrAddObjInObj(nextPathComponent, &out);
        addToObjectNoArrays(tag, val, state, *innerObj, idx + 1);
    });
}

void addToObject(value::Object& obj, AddToDocumentState& state);
void addToArray(value::Array& arr, AddToDocumentState& state) {
    size_t index = state.arrInfoReader.takeNumber();
    auto ensureArraySize = [&]() {
        while (arr.size() <= index) {
            arr.push_back(kPlaceHolderType, kPlaceHolderValue);
        }
    };

    while (!state.done()) {
        switch (auto nextChar = state.arrInfoReader.takeNextChar()) {
            case '+': {
                auto numToSkip = state.arrInfoReader.takeNumber();
                index += numToSkip;
                break;
            }
            case '|':
            case 'o': {
                const size_t repeats = state.arrInfoReader.takeNumber();

                // Grow the array once up front.
                auto insertAt = index;
                index += repeats;
                ensureArraySize();
                index++;
                for (; insertAt < index; insertAt++) {
                    invariant(insertAt < arr.size());

                    auto [tag, val] = [nextChar, &state]() {
                        if (nextChar == '|') {
                            return state.extractAndCopyValue();
                        } else {
                            invariant(nextChar == 'o');
                            return value::makeNewObject();
                        }
                    }();
                    if (state.atLastPathComponent()) {
                        // At this point we are inserting a leaf value.
                        dassert(arr.getAt(insertAt).first == kPlaceHolderType);
                        arr.setAt(insertAt, tag, val);
                    } else {
                        // This is valid on initialized elements when the subobject contains more
                        // than one member.
                        auto* subObj = findOrAddObjInArr(insertAt, &arr);
                        addToObjectNoArrays(tag, val, state, *subObj, 0);
                    }
                }
                break;
            }
            case '{': {
                ensureArraySize();
                auto* innerObj = findOrAddObjInArr(index, &arr);
                addToObject(*innerObj, state);
                index++;
                break;
            }
            case '[': {
                ensureArraySize();
                auto* innerArr = findOrAddArrInArr(index, &arr);
                addToArray(*innerArr, state);
                index++;
                break;
            }
            case ']':
                return;

            default:
                LOGV2_FATAL(6496300,
                            "Unexpected char in array info: {unexpectedChar}. Full arrInfo: {info}",
                            "unexpectedChar"_attr = nextChar,
                            "info"_attr = state.cell.arrInfo);
        }
    }
}

void addToObject(value::Object& obj, AddToDocumentState& state) {
    state.withNextPathComponent([&](StringData field) {
        switch (state.arrInfoReader.takeNextChar()) {
            case '{': {
                auto* innerObj = findOrAddObjInObj(field, &obj);
                addToObject(*innerObj, state);
                break;
            }
            case '[': {
                auto* innerArr = findOrAddArrInObj(field, &obj);
                addToArray(*innerArr, state);
                break;
            }

            default:
                LOGV2_FATAL(6496301,
                            "Unexpected char in array info {info}",
                            "info"_attr = state.cell.arrInfo);
        }
    });
}

void addEmptyObjectIfNotPresent(AddToDocumentState& state, value::Object& out) {
    // Add an object to the path.
    state.withNextPathComponent([&](StringData nextPathComponent) {
        auto* innerObj = findOrAddObjInObj(nextPathComponent, &out);
        if (!state.atLastPathComponent()) {
            addEmptyObjectIfNotPresent(state, *innerObj);
        }
    });
}
}  // namespace
void addCellToObject(TranslatedCell& cell, value::Object& out) {
    AddToDocumentState state{cell, ArrInfoReader{cell.arrInfo}};

    if (cell.arrInfo.empty()) {
        if (cell.moreValues()) {
            auto [tag, val] = cell.nextValue();
            auto [copyTag, copyVal] = value::copyValue(tag, val);
            addToObjectNoArrays(copyTag, copyVal, state, out, 0);
        } else {
            addEmptyObjectIfNotPresent(state, out);
        }

        invariant(state.done());
        return;
    }

    addToObject(out, state);
    invariant(!state.arrInfoReader.moreExplicitComponents());
}
}  // namespace mongo::sbe
