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

#include <absl/container/inlined_vector.h>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <cstring>
#include <map>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/shared_buffer.h"

namespace mongo::sbe {
template <typename O>
MakeObjStageBase<O>::MakeObjStageBase(std::unique_ptr<PlanStage> input,
                                      value::SlotId objSlot,
                                      boost::optional<value::SlotId> rootSlot,
                                      boost::optional<FieldBehavior> fieldBehavior,
                                      std::vector<std::string> fields,
                                      std::vector<std::string> projectFields,
                                      value::SlotVector projectVars,
                                      bool forceNewObject,
                                      bool returnOldObject,
                                      PlanNodeId planNodeId,
                                      bool participateInTrialRunTracking)
    : PlanStage(O::stageName, planNodeId, participateInTrialRunTracking),
      _objSlot(objSlot),
      _rootSlot(rootSlot),
      _fieldBehavior(fieldBehavior),
      _fields(std::move(fields)),
      _projectFields(std::move(projectFields)),
      _fieldNames(buildFieldNames(_fields, _projectFields)),
      _projectVars(std::move(projectVars)),
      _forceNewObject(forceNewObject),
      _returnOldObject(returnOldObject) {
    _children.emplace_back(std::move(input));
    invariant(_projectVars.size() == _projectFields.size());
    invariant(static_cast<bool>(rootSlot) == static_cast<bool>(fieldBehavior));
}

template <typename O>
MakeObjStageBase<O>::MakeObjStageBase(std::unique_ptr<PlanStage> input,
                                      value::SlotId objSlot,
                                      boost::optional<value::SlotId> rootSlot,
                                      boost::optional<FieldBehavior> fieldBehavior,
                                      OrderedPathSet fields,
                                      OrderedPathSet projectFields,
                                      value::SlotVector projectVars,
                                      bool forceNewObject,
                                      bool returnOldObject,
                                      PlanNodeId planNodeId)
    : MakeObjStageBase<O>::MakeObjStageBase(
          std::move(input),
          objSlot,
          rootSlot,
          fieldBehavior,
          std::vector<std::string>(fields.begin(), fields.end()),
          std::vector<std::string>(projectFields.begin(), projectFields.end()),
          std::move(projectVars),
          forceNewObject,
          returnOldObject,
          planNodeId) {}

template <typename O>
std::unique_ptr<PlanStage> MakeObjStageBase<O>::clone() const {
    return std::make_unique<MakeObjStageBase<O>>(_children[0]->clone(),
                                                 _objSlot,
                                                 _rootSlot,
                                                 _fieldBehavior,
                                                 _fields,
                                                 _projectFields,
                                                 _projectVars,
                                                 _forceNewObject,
                                                 _returnOldObject,
                                                 _commonStats.nodeId,
                                                 _participateInTrialRunTracking);
}

template <typename O>
void MakeObjStageBase<O>::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    if (_rootSlot) {
        _root = _children[0]->getAccessor(ctx, *_rootSlot);
    }

    for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
        auto& p = _projectFields[idx];
        _projects.emplace_back(p, _children[0]->getAccessor(ctx, _projectVars[idx]));
    }

    _visited.resize(_projectFields.size());

    _compiled = true;
}

template <typename O>
value::SlotAccessor* MakeObjStageBase<O>::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled && slot == _objSlot) {
        return &_obj;
    } else {
        return _children[0]->getAccessor(ctx, slot);
    }
}

template <typename O>
void MakeObjStageBase<O>::projectField(value::Object* obj, size_t idx) {
    const auto& p = _projects[idx];

    auto [tag, val] = p.second->getViewOfValue();
    if (tag != value::TypeTags::Nothing) {
        auto [tagCopy, valCopy] = value::copyValue(tag, val);
        obj->push_back(p.first, tagCopy, valCopy);
    }
}

template <typename O>
void MakeObjStageBase<O>::projectField(UniqueBSONObjBuilder* bob, size_t idx) {
    const auto& p = _projects[idx];

    auto [tag, val] = p.second->getViewOfValue();
    bson::appendValueToBsonObj(*bob, p.first, tag, val);
}

template <typename O>
void MakeObjStageBase<O>::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
}

template <>
void MakeObjStageBase<MakeObjOutputType::Object>::produceObject() {
    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);

    _obj.reset(tag, val);

    memset(_visited.data(), 0, _projectFields.size());

    const bool isInclusion = _fieldBehavior == FieldBehavior::keep;

    if (_root) {
        auto [tag, val] = _root->getViewOfValue();

        size_t numFieldsRemaining = _fields.size() + _projectFields.size();
        size_t numComputedFieldsRemaining = _projectFields.size();

        const size_t numFieldsRemainingThreshold = isInclusion ? 1 : 0;

        if (tag == value::TypeTags::bsonObject) {
            auto be = value::bitcastTo<const char*>(val);
            const auto size = ConstDataView(be).read<LittleEndian<uint32_t>>();
            const auto end = be + size;

            // Skip document length.
            be += sizeof(int32_t);

            // Simple heuristic to approximate the number of fields in '_root'.
            size_t approxNumFieldsInRoot = size / 16;

            // If the field behaviour is 'keep', then we know that the output will have exactly
            // '_fields.size() + _projectFields.size()' fields. Otherwise we use '_fields.size()'
            // plus the approximated number of fields in '_root' to approximate the number of
            // fields in the output object. (Note: we don't subtract '_projectFields.size()'
            // from the result in the latter case, as it might lead to a negative number.)
            size_t approxNumOutputFields = isInclusion ? _fields.size() + _projectFields.size()
                                                       : _fields.size() + approxNumFieldsInRoot;
            obj->reserve(approxNumOutputFields);

            // Loop over _root's fields until numFieldsRemaining - numComputedFieldsRemaining == 0
            // AND until one of the follow is true:
            //   (1) numComputedFieldsRemaining == 1 and isInclusion == true; -OR-
            //   (2) numComputedFieldsRemaining == 0.
            if (numFieldsRemaining > numFieldsRemainingThreshold ||
                numFieldsRemaining != numComputedFieldsRemaining) {
                while (be != end - 1) {
                    auto sv = bson::fieldNameAndLength(be);
                    auto [found, projectIdx] = lookupField(sv);

                    if (projectIdx == std::numeric_limits<size_t>::max()) {
                        if (found == isInclusion) {
                            auto [fieldTag, fieldVal] = bson::convertFrom<true>(be, end, sv.size());
                            auto [copyTag, copyVal] = value::copyValue(fieldTag, fieldVal);
                            obj->push_back(sv, copyTag, copyVal);
                        }

                        numFieldsRemaining -= found;
                    } else {
                        projectField(obj, projectIdx);
                        _visited[projectIdx] = 1;
                        --numFieldsRemaining;
                        --numComputedFieldsRemaining;
                    }

                    if (numFieldsRemaining <= numFieldsRemainingThreshold &&
                        numFieldsRemaining == numComputedFieldsRemaining) {
                        if (!isInclusion) {
                            be = bson::advance(be, sv.size());
                        }

                        break;
                    }

                    be = bson::advance(be, sv.size());
                }
            }

            // If this is an exclusion projection and 'be' has not reached the end of the input
            // object, copy over the remaining fields from the input object into 'bob'.
            if (!isInclusion) {
                while (be != end - 1) {
                    auto sv = bson::fieldNameAndLength(be);

                    auto [fieldTag, fieldVal] = bson::convertFrom<true>(be, end, sv.size());
                    auto [copyTag, copyVal] = value::copyValue(fieldTag, fieldVal);
                    obj->push_back(sv, copyTag, copyVal);

                    be = bson::advance(be, sv.size());
                }
            }
        } else if (tag == value::TypeTags::Object) {
            auto objRoot = value::getObjectView(val);
            size_t idx = 0;

            // If the field behaviour is 'keep', then we know that the output will have exactly
            // '_fields.size() + _projectFields.size()' fields. Otherwise use '_fields.size()'
            // plus the number of fields in '_root' to approximate the number of fields in the
            // output object. (Note: we don't subtract '_projectFields.size()' from the result
            // in the latter case, as it might lead to a negative number.)
            size_t approxNumOutputFields = isInclusion ? _fields.size() + _projectFields.size()
                                                       : _fields.size() + objRoot->size();
            obj->reserve(approxNumOutputFields);

            // Loop over _root's fields until numFieldsRemaining - numComputedFieldsRemaining == 0
            // AND until one of the follow is true:
            //   (1) numComputedFieldsRemaining == 1 and isInclusion == true; -OR-
            //   (2) numComputedFieldsRemaining == 0.
            if (numFieldsRemaining > numFieldsRemainingThreshold ||
                numFieldsRemaining != numComputedFieldsRemaining) {
                for (idx = 0; idx < objRoot->size(); ++idx) {
                    auto sv = StringData(objRoot->field(idx));
                    auto [found, projectIdx] = lookupField(sv);

                    if (projectIdx == std::numeric_limits<size_t>::max()) {
                        if (found == isInclusion) {
                            auto [fieldTag, fieldVal] = objRoot->getAt(idx);
                            auto [copyTag, copyVal] = value::copyValue(fieldTag, fieldVal);
                            obj->push_back(sv, copyTag, copyVal);
                        }

                        numFieldsRemaining -= found;
                    } else {
                        projectField(obj, projectIdx);
                        _visited[projectIdx] = 1;
                        --numFieldsRemaining;
                        --numComputedFieldsRemaining;
                    }

                    if (numFieldsRemaining <= numFieldsRemainingThreshold &&
                        numFieldsRemaining == numComputedFieldsRemaining) {
                        ++idx;
                        break;
                    }
                }
            }

            // If this is an exclusion projection and 'idx' has not reached the end of the input
            // object, copy over the remaining fields from the input object into 'bob'.
            if (!isInclusion) {
                for (; idx < objRoot->size(); ++idx) {
                    auto sv = StringData(objRoot->field(idx));
                    auto [fieldTag, fieldVal] = objRoot->getAt(idx);
                    auto [copyTag, copyVal] = value::copyValue(fieldTag, fieldVal);

                    obj->push_back(sv, copyTag, copyVal);
                }
            }
        } else {
            for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
                projectField(obj, idx);
            }
            // If the result is non empty object return it.
            if (obj->size() || _forceNewObject) {
                return;
            }
            // Now we have to make a decision - return Nothing or the original _root.
            if (!_returnOldObject) {
                _obj.reset(false, value::TypeTags::Nothing, 0);
            } else {
                // _root is not an object return it unmodified.
                _obj.reset(false, tag, val);
            }
            return;
        }
    }
    for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
        if (!_visited[idx]) {
            projectField(obj, idx);
        }
    }
}

template <>
void MakeObjStageBase<MakeObjOutputType::BsonObject>::produceObject() {
    UniqueBSONObjBuilder bob;

    auto finish = [this, &bob]() {
        bob.doneFast();
        char* data = bob.bb().release().release();
        _obj.reset(value::TypeTags::bsonObject, value::bitcastFrom<char*>(data));
    };

    memset(_visited.data(), 0, _projectFields.size());

    const bool isInclusion = _fieldBehavior == FieldBehavior::keep;

    if (_root) {
        auto [tag, val] = _root->getViewOfValue();

        size_t numFieldsRemaining = _fields.size() + _projectFields.size();
        size_t numComputedFieldsRemaining = _projectFields.size();

        const size_t numFieldsRemainingThreshold = isInclusion ? 1 : 0;

        if (tag == value::TypeTags::bsonObject) {
            auto be = value::bitcastTo<const char*>(val);
            const auto size = ConstDataView(be).read<LittleEndian<uint32_t>>();
            const auto end = be + size;

            // Skip document length.
            be += sizeof(int32_t);

            // Loop over _root's fields until numFieldsRemaining - numComputedFieldsRemaining == 0
            // AND until one of the follow is true:
            //   (1) numComputedFieldsRemaining == 1 and isInclusion == true; -OR-
            //   (2) numComputedFieldsRemaining == 0.
            if (numFieldsRemaining > numFieldsRemainingThreshold ||
                numFieldsRemaining != numComputedFieldsRemaining) {
                while (be != end - 1) {
                    const char* nextBe = nullptr;

                    auto sv = bson::fieldNameAndLength(be);
                    auto [found, projectIdx] = lookupField(sv);

                    if (projectIdx == std::numeric_limits<size_t>::max()) {
                        if (found == isInclusion) {
                            nextBe = bson::advance(be, sv.size());
                            bob.append(BSONElement(be, sv.size() + 1, nextBe - be));
                        }

                        numFieldsRemaining -= found;
                    } else {
                        projectField(&bob, projectIdx);
                        _visited[projectIdx] = 1;
                        --numFieldsRemaining;
                        --numComputedFieldsRemaining;
                    }

                    if (numFieldsRemaining <= numFieldsRemainingThreshold &&
                        numFieldsRemaining == numComputedFieldsRemaining) {
                        if (!isInclusion) {
                            be = nextBe ? nextBe : bson::advance(be, sv.size());
                        }

                        break;
                    }

                    be = nextBe ? nextBe : bson::advance(be, sv.size());
                }
            }

            // If this is an exclusion projection and 'be' has not reached the end of the input
            // object, copy over the remaining fields from the input object into 'bob'.
            if (!isInclusion) {
                while (be != end - 1) {
                    auto sv = bson::fieldNameAndLength(be);
                    auto nextBe = bson::advance(be, sv.size());

                    bob.append(BSONElement(be, sv.size() + 1, nextBe - be));

                    be = nextBe;
                }
            }
        } else if (tag == value::TypeTags::Object) {
            auto objRoot = value::getObjectView(val);
            size_t idx = 0;

            // Loop over _root's fields until numFieldsRemaining - numComputedFieldsRemaining == 0
            // AND until one of the follow is true:
            //   (1) numComputedFieldsRemaining == 1 and isInclusion == true; -OR-
            //   (2) numComputedFieldsRemaining == 0.
            if (numFieldsRemaining > numFieldsRemainingThreshold ||
                numFieldsRemaining != numComputedFieldsRemaining) {
                for (idx = 0; idx < objRoot->size(); ++idx) {
                    auto sv = StringData(objRoot->field(idx));
                    auto [found, projectIdx] = lookupField(sv);

                    if (projectIdx == std::numeric_limits<size_t>::max()) {
                        if (found == isInclusion) {
                            auto [fieldTag, fieldVal] = objRoot->getAt(idx);
                            bson::appendValueToBsonObj(bob, sv, fieldTag, fieldVal);
                        }

                        numFieldsRemaining -= found;
                    } else {
                        projectField(&bob, projectIdx);
                        _visited[projectIdx] = 1;
                        --numFieldsRemaining;
                        --numComputedFieldsRemaining;
                    }

                    if (numFieldsRemaining <= numFieldsRemainingThreshold &&
                        numFieldsRemaining == numComputedFieldsRemaining) {
                        ++idx;
                        break;
                    }
                }
            }

            // If this is an exclusion projection and 'idx' has not reached the end of the input
            // object, copy over the remaining fields from the input object into 'bob'.
            if (!isInclusion) {
                for (; idx < objRoot->size(); ++idx) {
                    auto sv = StringData(objRoot->field(idx));
                    auto [fieldTag, fieldVal] = objRoot->getAt(idx);

                    bson::appendValueToBsonObj(bob, sv, fieldTag, fieldVal);
                }
            }
        } else {
            for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
                projectField(&bob, idx);
            }
            // If the result is non empty object return it.
            if (!bob.asTempObj().isEmpty() || _forceNewObject) {
                finish();
                return;
            }
            // Now we have to make a decision - return Nothing or the original _root.
            if (!_returnOldObject) {
                _obj.reset(false, value::TypeTags::Nothing, 0);
            } else {
                // _root is not an object return it unmodified.
                _obj.reset(false, tag, val);
            }

            return;
        }
    }
    for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
        if (!_visited[idx]) {
            projectField(&bob, idx);
        }
    }
    finish();
}

template <typename O>
PlanState MakeObjStageBase<O>::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call getNext() on our child so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the getNext() call.
    disableSlotAccess();
    auto state = _children[0]->getNext();

    if (state == PlanState::ADVANCED) {
        produceObject();
    }
    return trackPlanState(state);
}

template <typename O>
void MakeObjStageBase<O>::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

template <typename O>
std::unique_ptr<PlanStageStats> MakeObjStageBase<O>::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("objSlot", static_cast<long long>(_objSlot));
        if (_rootSlot) {
            bob.appendNumber("rootSlot", static_cast<long long>(*_rootSlot));
        }
        if (_fieldBehavior) {
            bob.append("fieldBehavior", *_fieldBehavior == FieldBehavior::drop ? "drop" : "keep");
        }
        bob.append("fields", _fields);
        bob.append("projectFields", _projectFields);
        bob.append("projectSlots", _projectVars.begin(), _projectVars.end());
        bob.append("forceNewObject", _forceNewObject);
        bob.append("returnOldObject", _returnOldObject);
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

template <typename O>
const SpecificStats* MakeObjStageBase<O>::getSpecificStats() const {
    return nullptr;
}

template <typename O>
std::vector<DebugPrinter::Block> MakeObjStageBase<O>::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _objSlot);

    if (_rootSlot) {
        DebugPrinter::addIdentifier(ret, *_rootSlot);

        ret.emplace_back(DebugPrinter::Block("[`"));
        for (size_t idx = 0; idx < _fields.size(); ++idx) {
            if (idx) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }

            DebugPrinter::addIdentifier(ret, _fields[idx]);
        }
        ret.emplace_back(DebugPrinter::Block("`]"));

        ret.emplace_back(*_fieldBehavior == FieldBehavior::drop ? "drop" : "keep");
    }

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _projectFields[idx]);
        ret.emplace_back("=");
        DebugPrinter::addIdentifier(ret, _projectVars[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(_forceNewObject ? "true" : "false");
    ret.emplace_back(_returnOldObject ? "true" : "false");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

template <typename O>
size_t MakeObjStageBase<O>::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_fields);
    size += size_estimator::estimate(_projectFields);
    size += size_estimator::estimate(_projectVars);
    return size;
}

template <typename O>
void MakeObjStageBase<O>::doSaveState(bool relinquishCursor) {
    if (!relinquishCursor) {
        return;
    }

    prepareForYielding(_obj, slotsAccessible());
}

// Explicit template instantiations.
template class MakeObjStageBase<MakeObjOutputType::Object>;
template class MakeObjStageBase<MakeObjOutputType::BsonObject>;
}  // namespace mongo::sbe
