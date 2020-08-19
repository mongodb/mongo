/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <iterator>

#include "mongo/base/status.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/pipeline/variable_validation.h"

namespace mongo::c_node_validation {
namespace {

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsInclusionAssumed(const Iter& iter,
                                                                const EndFun& isEnd);
template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsExclusionAssumed(const Iter& iter,
                                                                const EndFun& isEnd);

auto isInclusionField(const CNode& project) {
    if (project.isInclusionKeyValue())
        // This is an inclusion Key.
        return true;
    else if (stdx::holds_alternative<KeyValue>(project.payload) ||
             stdx::holds_alternative<CompoundExclusionKey>(project.payload))
        // This is an exclusion Key.
        return false;
    else
        // This is an arbitrary expression to produce a computed field (this counts as inclusion).
        return true;
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsInclusionConfirmed(const Iter& iter,
                                                                  const EndFun& isEnd) {
    if (!isEnd(iter)) {
        if (CNode::fieldnameIsId(iter->first)) {
            return processAdditionalFieldsInclusionConfirmed(std::next(iter), isEnd);
        } else {
            if (isInclusionField(iter->second))
                return processAdditionalFieldsInclusionConfirmed(std::next(iter), isEnd);
            else
                return Status{ErrorCodes::FailedToParse,
                              "$project containing inclusion and/or computed fields must "
                              "contain no exclusion fields"};
        }
    } else {
        return IsInclusion::yes;
    }
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsExclusionConfirmed(const Iter& iter,
                                                                  const EndFun& isEnd) {
    if (!isEnd(iter)) {
        if (CNode::fieldnameIsId(iter->first)) {
            return processAdditionalFieldsExclusionConfirmed(std::next(iter), isEnd);
        } else {
            if (isInclusionField(iter->second))
                return Status{ErrorCodes::FailedToParse,
                              "$project containing exclusion fields must contain no "
                              "inclusion and/or computed fields"};
            else
                return processAdditionalFieldsExclusionConfirmed(std::next(iter), isEnd);
        }
    } else {
        return IsInclusion::no;
    }
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsWhenAssuming(const Iter& iter, const EndFun& isEnd) {
    if (CNode::fieldnameIsId(iter->first)) {
        if (isInclusionField(iter->second))
            return processAdditionalFieldsInclusionAssumed(std::next(iter), isEnd);
        else
            return processAdditionalFieldsExclusionAssumed(std::next(iter), isEnd);
    } else {
        if (isInclusionField(iter->second))
            return processAdditionalFieldsInclusionConfirmed(std::next(iter), isEnd);
        else
            return processAdditionalFieldsExclusionConfirmed(std::next(iter), isEnd);
    }
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsInclusionAssumed(const Iter& iter,
                                                                const EndFun& isEnd) {
    if (!isEnd(iter))
        return processAdditionalFieldsWhenAssuming(iter, isEnd);
    else
        return IsInclusion::yes;
}

template <typename Iter, typename EndFun>
StatusWith<IsInclusion> processAdditionalFieldsExclusionAssumed(const Iter& iter,
                                                                const EndFun& isEnd) {
    if (!isEnd(iter))
        return processAdditionalFieldsWhenAssuming(iter, isEnd);
    else
        return IsInclusion::no;
}

}  // namespace

StatusWith<IsInclusion> validateProjectionAsInclusionOrExclusion(const CNode& projects) {
    return processAdditionalFieldsInclusionAssumed(
        projects.objectChildren().cbegin(),
        [&](auto&& iter) { return iter == projects.objectChildren().cend(); });
}

Status validateVariableName(std::string varStr) {
    // The grammar removes the first two '$' characters.
    const StringData varName = varStr.substr(0, varStr.find('.'));
    try {
        variableValidation::validateNameForUserRead(varName);
    } catch (AssertionException& ae) {
        return Status(ae.code(), ae.reason());
    }
    return Status::OK();
}

}  // namespace mongo::c_node_validation
