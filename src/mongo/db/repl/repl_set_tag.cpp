/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_set_tag.h"

#include <algorithm>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/sequence_util.h"

namespace mongo {
namespace repl {

bool ReplSetTag::operator==(const ReplSetTag& other) const {
    return _keyIndex == other._keyIndex && _valueIndex == other._valueIndex;
}

bool ReplSetTag::operator!=(const ReplSetTag& other) const {
    return !(*this == other);
}

void ReplSetTagPattern::addTagCountConstraint(int32_t keyIndex, int32_t minCount) {
    const std::vector<TagCountConstraint>::iterator iter = std::find_if(
        _constraints.begin(),
        _constraints.end(),
        stdx::bind(std::equal_to<int32_t>(),
                   keyIndex,
                   stdx::bind(&TagCountConstraint::getKeyIndex, stdx::placeholders::_1)));
    if (iter == _constraints.end()) {
        _constraints.push_back(TagCountConstraint(keyIndex, minCount));
    } else if (iter->getMinCount() < minCount) {
        *iter = TagCountConstraint(keyIndex, minCount);
    }
}

ReplSetTagPattern::TagCountConstraint::TagCountConstraint(int32_t keyIndex, int32_t minCount)
    : _keyIndex(keyIndex), _minCount(minCount) {}

ReplSetTagMatch::ReplSetTagMatch(const ReplSetTagPattern& pattern) {
    for (ReplSetTagPattern::ConstraintIterator iter = pattern.constraintsBegin();
         iter != pattern.constraintsEnd();
         ++iter) {
        _boundTagValues.push_back(BoundTagValue(*iter));
    }
}

bool ReplSetTagMatch::update(const ReplSetTag& tag) {
    const std::vector<BoundTagValue>::iterator iter =
        std::find_if(_boundTagValues.begin(),
                     _boundTagValues.end(),
                     stdx::bind(std::equal_to<int32_t>(),
                                tag.getKeyIndex(),
                                stdx::bind(&BoundTagValue::getKeyIndex, stdx::placeholders::_1)));
    if (iter != _boundTagValues.end()) {
        if (!sequenceContains(iter->boundValues, tag.getValueIndex())) {
            iter->boundValues.push_back(tag.getValueIndex());
        }
    }
    return isSatisfied();
}

bool ReplSetTagMatch::isSatisfied() const {
    const std::vector<BoundTagValue>::const_iterator iter =
        std::find_if(_boundTagValues.begin(),
                     _boundTagValues.end(),
                     stdx::bind(std::logical_not<bool>(),
                                stdx::bind(&BoundTagValue::isSatisfied, stdx::placeholders::_1)));
    return iter == _boundTagValues.end();
}

bool ReplSetTagMatch::BoundTagValue::isSatisfied() const {
    return constraint.getMinCount() <= int32_t(boundValues.size());
}

ReplSetTag ReplSetTagConfig::makeTag(StringData key, StringData value) {
    int32_t keyIndex = _findKeyIndex(key);
    if (size_t(keyIndex) == _tagData.size()) {
        _tagData.push_back(make_pair(key.toString(), ValueVector()));
    }
    ValueVector& values = _tagData[keyIndex].second;
    for (size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
        if (values[valueIndex] != value)
            continue;
        return ReplSetTag(keyIndex, int32_t(valueIndex));
    }
    values.push_back(value.toString());
    return ReplSetTag(keyIndex, int32_t(values.size()) - 1);
}

ReplSetTag ReplSetTagConfig::findTag(StringData key, StringData value) const {
    int32_t keyIndex = _findKeyIndex(key);
    if (size_t(keyIndex) == _tagData.size())
        return ReplSetTag(-1, -1);
    const ValueVector& values = _tagData[keyIndex].second;
    for (size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
        if (values[valueIndex] == value) {
            return ReplSetTag(keyIndex, int32_t(valueIndex));
        }
    }
    return ReplSetTag(-1, -1);
}

ReplSetTagPattern ReplSetTagConfig::makePattern() const {
    return ReplSetTagPattern();
}

Status ReplSetTagConfig::addTagCountConstraintToPattern(ReplSetTagPattern* pattern,
                                                        StringData tagKey,
                                                        int32_t minCount) const {
    int32_t keyIndex = _findKeyIndex(tagKey);
    if (size_t(keyIndex) == _tagData.size()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "No replica set tag key " << tagKey << " in config");
    }
    pattern->addTagCountConstraint(keyIndex, minCount);
    return Status::OK();
}

int32_t ReplSetTagConfig::_findKeyIndex(StringData key) const {
    size_t i;
    for (i = 0; i < _tagData.size(); ++i) {
        if (_tagData[i].first == key) {
            break;
        }
    }
    return int32_t(i);
}

std::string ReplSetTagConfig::getTagKey(const ReplSetTag& tag) const {
    invariant(tag.isValid() && size_t(tag.getKeyIndex()) < _tagData.size());
    return _tagData[tag.getKeyIndex()].first;
}

std::string ReplSetTagConfig::getTagValue(const ReplSetTag& tag) const {
    invariant(tag.isValid() && size_t(tag.getKeyIndex()) < _tagData.size());
    const ValueVector& values = _tagData[tag.getKeyIndex()].second;
    invariant(tag.getValueIndex() >= 0 && size_t(tag.getValueIndex()) < values.size());
    return values[tag.getValueIndex()];
}

void ReplSetTagConfig::put(const ReplSetTag& tag, std::ostream& os) const {
    BSONObjBuilder builder;
    _appendTagKey(tag.getKeyIndex(), &builder);
    _appendTagValue(tag.getKeyIndex(), tag.getValueIndex(), &builder);
    os << builder.done();
}

void ReplSetTagConfig::put(const ReplSetTagPattern& pattern, std::ostream& os) const {
    BSONObjBuilder builder;
    BSONArrayBuilder allConstraintsBuilder(builder.subarrayStart("constraints"));
    for (ReplSetTagPattern::ConstraintIterator iter = pattern.constraintsBegin();
         iter != pattern.constraintsEnd();
         ++iter) {
        BSONObjBuilder constraintBuilder(allConstraintsBuilder.subobjStart());
        _appendConstraint(*iter, &constraintBuilder);
    }
    allConstraintsBuilder.doneFast();
    os << builder.done();
}

void ReplSetTagConfig::put(const ReplSetTagMatch& matcher, std::ostream& os) const {
    BSONObjBuilder builder;
    BSONArrayBuilder allBindingsBuilder(builder.subarrayStart("bindings"));
    for (size_t i = 0; i < matcher._boundTagValues.size(); ++i) {
        BSONObjBuilder bindingBuilder(allBindingsBuilder.subobjStart());
        _appendConstraint(matcher._boundTagValues[i].constraint, &bindingBuilder);
        BSONArrayBuilder boundValues(bindingBuilder.subarrayStart("boundValues"));
        for (size_t j = 0; j < matcher._boundTagValues[i].boundValues.size(); ++j) {
            BSONObjBuilder bvb(boundValues.subobjStart());
            _appendTagValue(matcher._boundTagValues[i].constraint.getKeyIndex(),
                            matcher._boundTagValues[i].boundValues[j],
                            &bvb);
        }
    }
    allBindingsBuilder.doneFast();
    os << builder.done();
}

void ReplSetTagConfig::_appendTagKey(int32_t keyIndex, BSONObjBuilder* builder) const {
    if (keyIndex < 0 || size_t(keyIndex) >= _tagData.size()) {
        builder->append("tagKey", int(keyIndex));
    } else {
        builder->append("tagKey", _tagData[keyIndex].first);
    }
}

void ReplSetTagConfig::_appendTagValue(int32_t keyIndex,
                                       int32_t valueIndex,
                                       BSONObjBuilder* builder) const {
    if (keyIndex < 0 || size_t(keyIndex) >= _tagData.size()) {
        builder->append("tagValue", valueIndex);
        return;
    }
    KeyValueVector::const_reference keyEntry = _tagData[keyIndex];
    if (valueIndex < 0 || size_t(valueIndex) < keyEntry.second.size()) {
        builder->append("tagValue", valueIndex);
    }
    builder->append("tagValue", keyEntry.second[valueIndex]);
}

void ReplSetTagConfig::_appendConstraint(const ReplSetTagPattern::TagCountConstraint& constraint,
                                         BSONObjBuilder* builder) const {
    _appendTagKey(constraint.getKeyIndex(), builder);
    builder->append("minCount", int(constraint.getMinCount()));
}


}  // namespace repl
}  // namespace mongo
