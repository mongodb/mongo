/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_Parser_inl_h
#define frontend_Parser_inl_h

#include "frontend/Parser.h"

#include "frontend/ParseMaps-inl.h"

namespace js {
namespace frontend {

template <typename ParseHandler>
bool
ParseContext<ParseHandler>::init(Parser<ParseHandler>& parser)
{
    if (!parser.generateBlockId(sc->staticScope(), &this->bodyid))
        return false;

    if (!decls_.init() || !lexdeps.ensureMap(sc->context)) {
        ReportOutOfMemory(sc->context);
        return false;
    }

    return true;
}

template <typename ParseHandler>
ParseContext<ParseHandler>::~ParseContext()
{
    // |*parserPC| pointed to this object.  Now that this object is about to
    // die, make |*parserPC| point to this object's parent.
    MOZ_ASSERT(*parserPC == this);
    *parserPC = this->oldpc;
}

} // namespace frontend
} // namespace js

#endif /* frontend_Parser_inl_h */
