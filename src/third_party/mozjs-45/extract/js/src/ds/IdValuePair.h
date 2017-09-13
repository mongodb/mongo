/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_IdValuePair_h
#define ds_IdValuePair_h

#include "jsapi.h"

#include "NamespaceImports.h"
#include "gc/Tracer.h"
#include "js/Id.h"
#include "js/TraceableVector.h"

namespace js {

struct IdValuePair
{
    Value value;
    jsid id;

    IdValuePair()
      : value(UndefinedValue()), id(JSID_EMPTY)
    {}
    explicit IdValuePair(jsid idArg)
      : value(UndefinedValue()), id(idArg)
    {}
    IdValuePair(jsid idArg, Value valueArg)
      : value(valueArg), id(idArg)
    {}

    void trace(JSTracer* trc) {
        TraceRoot(trc, &value, "IdValuePair::value");
        TraceRoot(trc, &id, "IdValuePair::id");
    }
};

using IdValueVector = TraceableVector<IdValuePair>;

} /* namespace js */

#endif /* ds_IdValuePair_h */
