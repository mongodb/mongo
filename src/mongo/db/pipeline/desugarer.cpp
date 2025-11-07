/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/desugarer.h"

namespace mongo {

void Desugarer::operator()() {
    auto srcItr = _sources.begin();
    while (srcItr != _sources.end()) {
        tassert(10978003, "Invalid desugarer iterator", srcItr->get());
        const auto& stage = *srcItr->get();

        // Check if the stage is desugarable by looking in the stageExpander map.
        if (auto stageExpanderItr = _stageExpanders.find(stage.getId());
            stageExpanderItr != _stageExpanders.end()) {
            srcItr = stageExpanderItr->second(this, srcItr, stage);
        } else {
            srcItr = std::next(srcItr);
        }
    }
}

DocumentSourceContainer::iterator Desugarer::replaceStageWith(
    DocumentSourceContainer::iterator itr,
    std::list<boost::intrusive_ptr<DocumentSource>>&& newSources) {
    _sources.splice(itr, newSources);
    auto next = std::next(itr);
    _sources.erase(itr);
    return next;
}

}  // namespace mongo
