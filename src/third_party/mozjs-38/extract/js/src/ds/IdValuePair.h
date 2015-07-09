/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_IdValuePair_h
#define ds_IdValuePair_h

#include "NamespaceImports.h"

#include "js/Id.h"

namespace js {

struct IdValuePair
{
    jsid id;
    Value value;

    IdValuePair() {}
    explicit IdValuePair(jsid idArg)
      : id(idArg), value(UndefinedValue())
    {}
};

} /* namespace js */

#endif /* ds_IdValuePair_h */
