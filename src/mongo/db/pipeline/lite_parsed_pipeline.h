/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <memory>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"

namespace mongo {

/**
 * A semi-parsed version of a Pipeline, parsed just enough to determine information like what
 * foreign collections are involved.
 */
class LiteParsedPipeline {
public:
    /**
     * Constructs a LiteParsedPipeline from the raw BSON stages given in 'request'.
     *
     * May throw a UserException if there is an invalid stage specification, although full
     * validation happens later, during Pipeline construction.
     */
    LiteParsedPipeline(const AggregationRequest& request) {
        _stageSpecs.reserve(request.getPipeline().size());

        for (auto&& rawStage : request.getPipeline()) {
            _stageSpecs.push_back(LiteParsedDocumentSource::parse(request, rawStage));
        }
    }

    /**
     * Returns all foreign namespaces referenced by stages within this pipeline, if any.
     */
    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const {
        stdx::unordered_set<NamespaceString> involvedNamespaces;
        for (auto&& spec : _stageSpecs) {
            auto stagesInvolvedNamespaces = spec->getInvolvedNamespaces();
            involvedNamespaces.insert(stagesInvolvedNamespaces.begin(),
                                      stagesInvolvedNamespaces.end());
        }
        return involvedNamespaces;
    }

    /**
     * Returns a list of the priviliges required for this pipeline.
     */
    PrivilegeVector requiredPrivileges(bool isMongos) const {
        PrivilegeVector requiredPrivileges;
        for (auto&& spec : _stageSpecs) {
            Privilege::addPrivilegesToPrivilegeVector(&requiredPrivileges,
                                                      spec->requiredPrivileges(isMongos));
        }

        return requiredPrivileges;
    }

    /**
     * Returns true if the pipeline begins with a $collStats stage.
     */
    bool startsWithCollStats() const {
        return !_stageSpecs.empty() && _stageSpecs.front()->isCollStats();
    }

    /**
     * Returns true if the pipeline has a $changeNotification stage.
     *
     * TODO SERVER-29506 Require $changeNotification to be the first stage.
     */
    bool hasChangeNotification() const {
        return std::any_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
            return spec->isChangeNotification();
        });
    }

private:
    std::vector<std::unique_ptr<LiteParsedDocumentSource>> _stageSpecs;
};

}  // namespace mongo
