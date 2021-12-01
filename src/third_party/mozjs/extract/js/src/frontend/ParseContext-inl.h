#ifndef frontend_ParseContext_inl_h
#define frontend_ParseContext_inl_h

#include "frontend/ParseContext.h"

namespace js {
namespace frontend {

template <>
inline bool
ParseContext::Statement::is<ParseContext::LabelStatement>() const
{
    return kind_ == StatementKind::Label;
}

template <>
inline bool
ParseContext::Statement::is<ParseContext::ClassStatement>() const
{
    return kind_ == StatementKind::Class;
}


inline JS::Result<Ok, ParseContext::BreakStatementError>
ParseContext::checkBreakStatement(PropertyName* label)
{
    // Labeled 'break' statements target the nearest labeled statements (could
    // be any kind) with the same label. Unlabeled 'break' statements target
    // the innermost loop or switch statement.
    if (label) {
        auto hasSameLabel = [&label](ParseContext::LabelStatement* stmt) {
            MOZ_ASSERT(stmt);
            return stmt->label() == label;
        };

        if (!findInnermostStatement<ParseContext::LabelStatement>(hasSameLabel))
            return mozilla::Err(ParseContext::BreakStatementError::LabelNotFound);

    } else {
        auto isBreakTarget = [](ParseContext::Statement* stmt) {
            return StatementKindIsUnlabeledBreakTarget(stmt->kind());
        };

        if (!findInnermostStatement(isBreakTarget))
            return mozilla::Err(ParseContext::BreakStatementError::ToughBreak);
    }

    return Ok();
}

inline JS::Result<Ok, ParseContext::ContinueStatementError>
ParseContext::checkContinueStatement(PropertyName* label)
{
    // Labeled 'continue' statements target the nearest labeled loop
    // statements with the same label. Unlabeled 'continue' statements target
    // the innermost loop statement.
    auto isLoop = [](ParseContext::Statement* stmt) {
        MOZ_ASSERT(stmt);
        return StatementKindIsLoop(stmt->kind());
    };

    if (!label) {
        // Unlabeled statement: we target the innermost loop, so make sure that
        // there is an innermost loop.
        if (!findInnermostStatement(isLoop))
            return mozilla::Err(ParseContext::ContinueStatementError::NotInALoop);
        return Ok();
    }

    // Labeled statement: targest the nearest labeled loop with the same label.
    ParseContext::Statement* stmt = innermostStatement();
    bool foundLoop = false; // True if we have encountered at least one loop.

    for (;;) {
        stmt = ParseContext::Statement::findNearest(stmt, isLoop);
        if (!stmt)
            return foundLoop ? mozilla::Err(ParseContext::ContinueStatementError::LabelNotFound)
                             : mozilla::Err(ParseContext::ContinueStatementError::NotInALoop);

        foundLoop = true;

        // Is it labeled by our label?
        stmt = stmt->enclosing();
        while (stmt && stmt->is<ParseContext::LabelStatement>()) {
            if (stmt->as<ParseContext::LabelStatement>().label() == label)
                return Ok();

            stmt = stmt->enclosing();
        }
    }
}

}
}

#endif // frontend_ParseContext_inl_h
