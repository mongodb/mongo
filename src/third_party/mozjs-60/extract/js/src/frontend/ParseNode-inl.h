/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseNode_inl_h
#define frontend_ParseNode_inl_h

#include "frontend/ParseNode.h"

#include "frontend/SharedContext.h"

namespace js {
namespace frontend {

inline PropertyName*
ParseNode::name() const
{
    MOZ_ASSERT(isKind(ParseNodeKind::Function) || isKind(ParseNodeKind::Name));
    JSAtom* atom = isKind(ParseNodeKind::Function) ? pn_funbox->function()->explicitName() : pn_atom;
    return atom->asPropertyName();
}

inline JSAtom*
ParseNode::atom() const
{
    MOZ_ASSERT(isKind(ParseNodeKind::String));
    return pn_atom;
}

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ParseNode_inl_h */
