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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/document_transformation.h"
#include "mongo/util/modules.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::document_transformation {

namespace detail {

void describeProjectedPath(DocumentOperationVisitor& visitor, StringData path, bool isInclusion);

void describeComputedPath(DocumentOperationVisitor& visitor,
                          const std::string& path,
                          const boost::intrusive_ptr<Expression>& expr,
                          BSONDepthIndex depth);

}  // namespace detail

/**
 * Describe the list of included/excluded paths, prefixed by the optional prefix (joined with '.').
 * If 'isInclusion' is true, the caller must ensure that ReplaceRoot is provided to the visitor
 * first, since inclusions are not valid unless ReplaceRoot is present.
 */
template <typename It>
void describeProjectedPaths(
    DocumentOperationVisitor& visitor, It startIt, It endIt, StringData prefix, bool isInclusion) {
    for (; startIt != endIt; ++startIt) {
        auto path = FieldPath::getFullyQualifiedPath(prefix, *startIt);
        detail::describeProjectedPath(visitor, path, isInclusion);
    }
}

/**
 * Describe the list of computed paths and Expression pairs, prefixed by the optional prefix
 * (joined with '.').
 */
template <typename It>
void describeComputedPaths(DocumentOperationVisitor& visitor,
                           It startIt,
                           It endIt,
                           StringData prefix) {
    BSONDepthIndex depth = prefix.empty() ? 0 : 1 + std::count(prefix.begin(), prefix.end(), '.');
    for (; startIt != endIt; ++startIt) {
        auto&& computedPair = *startIt;
        auto path = FieldPath::getFullyQualifiedPath(prefix, computedPair.first);
        detail::describeComputedPath(visitor, path, computedPair.second, depth);
    }
}


using GetModPathsReturn = DocumentSource::GetModPathsReturn;
using GetModPathsReturnType = GetModPathsReturn::Type;

/**
 * Provides an interface similar to DescribesDocumentTransformation via a GetModPathsReturn.
 * The conversion is non-lossy, except for the case of kNotSupported, which is treated the same as
 * kAllPaths.
 */
void describeGetModPathsReturn(DocumentOperationVisitor& visitor,
                               const GetModPathsReturnType& type,
                               const OrderedPathSet& paths,
                               const StringMap<std::string>& renames,
                               const StringMap<std::string>& complexRenames);

/**
 * Provides an interface similar to DescribesDocumentTransformation via a GetModPathsReturn.
 */
inline void describeGetModPathsReturn(DocumentOperationVisitor& visitor,
                                      const GetModPathsReturn& modPaths) {
    describeGetModPathsReturn(
        visitor, modPaths.type, modPaths.paths, modPaths.renames, modPaths.complexRenames);
}

namespace detail {
/**
 * A stateful visitor which produces a GetModPathsReturn.
 *
 * This is a lossy conversion, since GetModPathsReturn cannot:
 * - report additional information about the paths
 * - report modified paths under kAllExcept (computed fields)
 * - report modified paths under kAllPaths (computed fields)
 * - report renames other than simple and complex renames (a.b.c: $x or x: $a.b.c)
 */
class GetModPathsReturnConverter : public DocumentOperationVisitor {
public:
    void operator()(const ReplaceRoot&) override;
    void operator()(const ModifyPath&) override;
    void operator()(const PreservePath&) override;
    void operator()(const RenamePath&) override;
    GetModPathsReturn done() &&;

private:
    GetModPathsReturnType type{GetModPathsReturnType::kNotSupported};
    OrderedPathSet paths;
    StringMap<std::string> renames;
    StringMap<std::string> complexRenames;
};
}  // namespace detail

GetModPathsReturn toGetModPathsReturn(const DescribesDocumentTransformation auto& t) {
    detail::GetModPathsReturnConverter c;
    t.describeTransformation(c);
    return std::move(c).done();
}

namespace detail {
/**
 * A path rename which knows about arrayness.
 */
class RenamePathWithFixedArrayness final : public RenamePath {
public:
    RenamePathWithFixedArrayness(StringData newPath,
                                 StringData oldPath,
                                 BSONDepthIndex newPathMaxArrayTraversals,
                                 BSONDepthIndex oldPathMaxArrayTraversals);

    BSONDepthIndex getNewPathMaxArrayTraversals() const override {
        return _newPathMaxArrayTraversals;
    }
    BSONDepthIndex getOldPathMaxArrayTraversals() const override {
        return _oldPathMaxArrayTraversals;
    }

private:
    const BSONDepthIndex _newPathMaxArrayTraversals;
    const BSONDepthIndex _oldPathMaxArrayTraversals;
};

/**
 * Creates a DescribesDocumentTransformation that applies 'inner' through a custom visitor
 * produced by 'makeVisitor(outputVisitor)'. Analogous to DocumentOperationVisitor::create().
 */
template <DescribesDocumentTransformation Inner, typename MakeVisitor>
auto wrapDocumentTransformation(const Inner& inner, MakeVisitor&& makeVisitor) {
    struct Result {
        const Inner& inner;
        std::decay_t<MakeVisitor> makeVisitor;
        void describeTransformation(DocumentOperationVisitor& visitor) const {
            document_transformation::describeTransformation(makeVisitor(visitor), inner);
        }
    };
    return Result{inner, std::forward<MakeVisitor>(makeVisitor)};
}
}  // namespace detail

/**
 * Returns a DescribesDocumentTransformation that overrides path renames to
 * 'RenamePathWithFixedArrayness' when 'canBeArray' says that there are no arrays involved.
 */
template <typename T, typename CanPathBeArray>
auto withArraynessInfo(const T& t, CanPathBeArray&& canPathBeArray) {
    auto dottedPrefix = [](StringData path) {
        return path.substr(0, path.rfind('.'));
    };

    auto noArraysInvolved = [canPathBeArray = std::forward<CanPathBeArray>(canPathBeArray),
                             dottedPrefix](const RenamePath& op) {
        if (op.getOldPathMaxArrayTraversals() > 0 &&
            canPathBeArray(dottedPrefix(op.getOldPath()))) {
            return false;
        }
        if (op.getNewPathMaxArrayTraversals() > 0 &&
            canPathBeArray(dottedPrefix(op.getNewPath()))) {
            return false;
        }
        return true;
    };

    return detail::wrapDocumentTransformation(
        t, [noArraysInvolved](DocumentOperationVisitor& visitor) {
            return OverloadedVisitor{
                [&](const auto& op) { visitor(op); },
                [&](const RenamePath& op) {
                    if (noArraysInvolved(op)) {
                        visitor(detail::RenamePathWithFixedArrayness{
                            op.getNewPath(), op.getOldPath(), 0, 0});
                    } else {
                        visitor(op);
                    }
                },
            };
        });
}

}  // namespace mongo::document_transformation
