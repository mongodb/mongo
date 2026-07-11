// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/document_transformation.h"
#include "mongo/util/modules.h"
#include "mongo/util/overloaded_visitor.h"

#include <string_view>

namespace mongo::document_transformation {

/**
 * A subclass for a modification which will never evaluate to an array and has the kEnsureObjects
 * prefix semantics.
 * Example:
 * {$lookup: {as: 'a.b.c'}} {$unwind: '$a.b.c'} - "$a.b.c" never evaluates to an array
 */
class NonArrayModifyPath final : public ModifyPath {
public:
    NonArrayModifyPath(std::string_view path, bool canLeafBeArray)
        : ModifyPath(path, ModifiedPrefixPolicy::kEnsureObjects), _canLeafBeArray(canLeafBeArray) {}

    bool canLeafBeArray() const override {
        return _canLeafBeArray;
    }

private:
    bool _canLeafBeArray;
};

namespace detail {

void describeProjectedPath(DocumentOperationVisitor& visitor,
                           std::string_view path,
                           bool isInclusion);

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
void describeProjectedPaths(DocumentOperationVisitor& visitor,
                            It startIt,
                            It endIt,
                            std::string_view prefix,
                            bool isInclusion) {
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
                           std::string_view prefix) {
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
    RenamePathWithFixedArrayness(std::string_view newPath,
                                 std::string_view oldPath,
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
    auto dottedPrefix = [](std::string_view path) {
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
