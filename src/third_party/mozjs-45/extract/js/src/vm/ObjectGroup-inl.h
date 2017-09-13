/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ObjectGroup_inl_h
#define vm_ObjectGroup_inl_h

#include "vm/ObjectGroup.h"

namespace js {

inline bool
ObjectGroup::needsSweep()
{
    // Note: this can be called off thread during compacting GCs, in which case
    // nothing will be running on the main thread.
    return generation() != zoneFromAnyThread()->types.generation;
}

inline void
ObjectGroup::maybeSweep(AutoClearTypeInferenceStateOnOOM* oom)
{
    if (needsSweep())
        sweep(oom);
}

inline ObjectGroupFlags
ObjectGroup::flags()
{
    maybeSweep(nullptr);
    return flagsDontCheckGeneration();
}

inline void
ObjectGroup::addFlags(ObjectGroupFlags flags)
{
    maybeSweep(nullptr);
    flags_ |= flags;
}

inline void
ObjectGroup::clearFlags(ObjectGroupFlags flags)
{
    maybeSweep(nullptr);
    flags_ &= ~flags;
}

inline bool
ObjectGroup::hasAnyFlags(ObjectGroupFlags flags)
{
    MOZ_ASSERT((flags & OBJECT_FLAG_DYNAMIC_MASK) == flags);
    return !!(this->flags() & flags);
}

inline bool
ObjectGroup::hasAllFlags(ObjectGroupFlags flags)
{
    MOZ_ASSERT((flags & OBJECT_FLAG_DYNAMIC_MASK) == flags);
    return (this->flags() & flags) == flags;
}

inline bool
ObjectGroup::unknownProperties()
{
    MOZ_ASSERT_IF(flags() & OBJECT_FLAG_UNKNOWN_PROPERTIES,
                  hasAllFlags(OBJECT_FLAG_DYNAMIC_MASK));
    return !!(flags() & OBJECT_FLAG_UNKNOWN_PROPERTIES);
}

inline bool
ObjectGroup::shouldPreTenure()
{
    return hasAnyFlags(OBJECT_FLAG_PRE_TENURE) && !unknownProperties();
}

inline bool
ObjectGroup::canPreTenure()
{
    return !unknownProperties();
}

inline bool
ObjectGroup::fromAllocationSite()
{
    return flags() & OBJECT_FLAG_FROM_ALLOCATION_SITE;
}

inline void
ObjectGroup::setShouldPreTenure(ExclusiveContext* cx)
{
    MOZ_ASSERT(canPreTenure());
    setFlags(cx, OBJECT_FLAG_PRE_TENURE);
}

inline TypeNewScript*
ObjectGroup::newScript()
{
    maybeSweep(nullptr);
    return newScriptDontCheckGeneration();
}

inline PreliminaryObjectArrayWithTemplate*
ObjectGroup::maybePreliminaryObjects()
{
    maybeSweep(nullptr);
    return maybePreliminaryObjectsDontCheckGeneration();
}

inline UnboxedLayout*
ObjectGroup::maybeUnboxedLayout()
{
    maybeSweep(nullptr);
    return maybeUnboxedLayoutDontCheckGeneration();
}

inline UnboxedLayout&
ObjectGroup::unboxedLayout()
{
    maybeSweep(nullptr);
    return unboxedLayoutDontCheckGeneration();
}

} // namespace js

#endif /* vm_ObjectGroup_inl_h */
