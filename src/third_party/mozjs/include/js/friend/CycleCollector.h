/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * APIs for integration with the cycle collector.
 */

#ifndef js_friend_CycleCollector_h
#define js_friend_CycleCollector_h

#include "jstypes.h"

#include "js/HeapAPI.h"  // JS::GCCellPtr

namespace JS {

using ShouldClearWeakRefTargetCallback = bool (*)(GCCellPtr ptr, void* data);

extern JS_PUBLIC_API void MaybeClearWeakRefTargets(
    JSRuntime* runtime, ShouldClearWeakRefTargetCallback callback, void* data);

}  // namespace JS

#endif  // js_friend_CycleCollector_h
