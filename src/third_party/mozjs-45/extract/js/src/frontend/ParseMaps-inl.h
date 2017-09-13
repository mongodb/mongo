/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseMaps_inl_h
#define frontend_ParseMaps_inl_h

#include "frontend/ParseMaps.h"

#include "jscntxtinlines.h"

namespace js {
namespace frontend {

template <class Map>
inline bool
AtomThingMapPtr<Map>::ensureMap(ExclusiveContext* cx)
{
    if (map_)
        return true;

    AutoLockForExclusiveAccess lock(cx);
    map_ = cx->parseMapPool().acquire<Map>();
    if (!map_)
        ReportOutOfMemory(cx);
    return !!map_;
}

template <class Map>
inline void
AtomThingMapPtr<Map>::releaseMap(ExclusiveContext* cx)
{
    if (!map_)
        return;

    AutoLockForExclusiveAccess lock(cx);
    cx->parseMapPool().release(map_);
    map_ = nullptr;
}

template <typename ParseHandler>
inline bool
AtomDecls<ParseHandler>::init()
{
    AutoLockForExclusiveAccess lock(cx);
    map = cx->parseMapPool().acquire<AtomDefnListMap>();
    return map;
}

template <typename ParseHandler>
inline
AtomDecls<ParseHandler>::~AtomDecls()
{
    if (map) {
        AutoLockForExclusiveAccess lock(cx);
        cx->parseMapPool().release(map);
    }
}

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ParseMaps_inl_h */
