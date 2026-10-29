// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_arity.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/util/modules.h"

#include <array>
#include <memory>
#include <string_view>
#include <utility>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * A MatchExpression that represents the ternary "conditional" operator.
 */
class InternalSchemaCondMatchExpression final
    : public FixedArityMatchExpression<InternalSchemaCondMatchExpression, 3> {
public:
    static constexpr std::string_view kName = "$_internalSchemaCond"sv;

    explicit InternalSchemaCondMatchExpression(
        std::array<std::unique_ptr<MatchExpression>, 3> expressions,
        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : FixedArityMatchExpression(
              MatchType::INTERNAL_SCHEMA_COND, std::move(expressions), std::move(annotation)) {}

    const MatchExpression* condition() const {
        return expressions()[0].get();
    }

    const MatchExpression* thenBranch() const {
        return expressions()[1].get();
    }

    const MatchExpression* elseBranch() const {
        return expressions()[2].get();
    }

    std::string_view name() const final {
        return kName;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void applyRename(const StringMap<std::string>& renameList) {
        if (renameList.empty()) {
            return;
        }

        expression::Renameables renameables;
        for (size_t i = 0; i < numChildren(); ++i) {
            auto* child = getChild(i);
            const bool canRename = expression::hasOnlyRenameableMatchExpressionChildren(
                *child, renameList, renameables);
            invariant(canRename);
        }
        expression::applyRenamesToExpression(renameList, &renameables);
    }

    bool hasRenameablePath(const StringMap<std::string>& renameList) const {
        if (renameList.empty()) {
            return true;
        }

        for (size_t i = 0; i < numChildren(); ++i) {
            auto* child = getChild(i);
            const bool canRename =
                expression::hasOnlyRenameableMatchExpressionChildren(*child, renameList);
            if (!canRename) {
                return false;
            }
        }
        return true;
    }
};

}  // namespace mongo
