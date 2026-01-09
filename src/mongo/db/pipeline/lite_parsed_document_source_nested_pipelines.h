/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

namespace mongo {

/**
 * Template for DocumentSources which can reference one or more child pipelines. This template is in
 * a separate header because it requires LiteParsedPipeline to be a complete type.
 */
template <typename Derived>
class LiteParsedDocumentSourceNestedPipelines : public LiteParsedDocumentSourceDefault<Derived> {
public:
    LiteParsedDocumentSourceNestedPipelines(const BSONElement& originalBson,
                                            boost::optional<NamespaceString> foreignNss,
                                            std::vector<LiteParsedPipeline> pipelines)
        : LiteParsedDocumentSourceDefault<Derived>(originalBson),
          _foreignNss(std::move(foreignNss)),
          _pipelines(std::move(pipelines)) {}

    LiteParsedDocumentSourceNestedPipelines(const BSONElement& originalBson,
                                            boost::optional<NamespaceString> foreignNss,
                                            boost::optional<LiteParsedPipeline> pipeline)
        : LiteParsedDocumentSourceNestedPipelines(
              originalBson, std::move(foreignNss), std::vector<LiteParsedPipeline>{}) {
        if (pipeline)
            _pipelines.emplace_back(std::move(pipeline.value()));
    }

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
        stdx::unordered_set<NamespaceString> involvedNamespaces;
        if (_foreignNss)
            involvedNamespaces.insert(*_foreignNss);

        for (auto&& pipeline : _pipelines) {
            const auto& involvedInSubPipe = pipeline.getInvolvedNamespaces();
            involvedNamespaces.insert(involvedInSubPipe.begin(), involvedInSubPipe.end());
        }
        return involvedNamespaces;
    }

    void getForeignExecutionNamespaces(
        stdx::unordered_set<NamespaceString>& nssSet) const override {
        for (auto&& pipeline : _pipelines) {
            auto nssVector = pipeline.getForeignExecutionNamespaces();
            for (const auto& nssOrUUID : nssVector) {
                tassert(6458500,
                        "nss expected to contain a NamespaceString",
                        nssOrUUID.isNamespaceString());
                nssSet.insert(nssOrUUID.nss());
            }
        }
    }

    bool isExemptFromIngressAdmissionControl() const override {
        return std::any_of(_pipelines.begin(), _pipelines.end(), [](auto&& pipeline) {
            return pipeline.isExemptFromIngressAdmissionControl();
        });
    }

    Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                          bool inMultiDocumentTransaction) const override {
        for (auto&& pipeline : _pipelines) {
            if (auto status =
                    pipeline.checkShardedForeignCollAllowed(nss, inMultiDocumentTransaction);
                !status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    const std::vector<LiteParsedPipeline>& getSubPipelines() const override {
        return _pipelines;
    }

    /**
     * Check the read concern constraints of all sub-pipelines. If the stage that owns the
     * sub-pipelines has its own constraints this should be overridden to take those into account.
     */
    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        // Assume that the document source holding the pipeline has no constraints of its own, so
        // return the strictest of the constraints on the sub-pipelines.
        auto result = ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        for (auto& pipeline : _pipelines) {
            result.merge(pipeline.sourcesSupportReadConcern(level, isImplicitDefault));
            // If both result statuses are already not OK, stop checking.
            if (!result.readConcernSupport.isOK() && !result.defaultReadConcernPermit.isOK()) {
                break;
            }
        }
        return result;
    }

    bool requiresAuthzChecks() const override {
        return true;
    }

protected:
    /**
     * Simple implementation that only gets the privileges needed by children pipelines.
     */
    PrivilegeVector requiredPrivilegesBasic(bool isMongos, bool bypassDocumentValidation) const {
        PrivilegeVector requiredPrivileges;
        for (auto&& pipeline : _pipelines) {
            Privilege::addPrivilegesToPrivilegeVector(
                &requiredPrivileges,
                pipeline.requiredPrivileges(isMongos, bypassDocumentValidation));
        }
        return requiredPrivileges;
    }

    boost::optional<NamespaceString> _foreignNss;
    std::vector<LiteParsedPipeline> _pipelines;
};

}  // namespace mongo
