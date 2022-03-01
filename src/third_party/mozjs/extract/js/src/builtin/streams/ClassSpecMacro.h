/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A JS_STREAMS_CLASS_SPEC macro for defining streams classes. */

#ifndef builtin_streams_ClassSpecMacro_h
#define builtin_streams_ClassSpecMacro_h

#include "gc/AllocKind.h"  // js::gc::AllocKind
#include "js/Class.h"  // js::ClassSpec, JSClass, JSCLASS_HAS_{CACHED_PROTO,RESERVED_SLOTS}, JS_NULL_CLASS_OPS
#include "js/ProtoKey.h"      // JSProto_*
#include "vm/GlobalObject.h"  // js::GenericCreate{Constructor,Prototype}

#define JS_STREAMS_CLASS_SPEC(cls, nCtorArgs, nSlots, specFlags, classFlags, \
                              classOps)                                      \
  const js::ClassSpec cls::classSpec_ = {                                    \
      js::GenericCreateConstructor<cls::constructor, nCtorArgs,              \
                                   js::gc::AllocKind::FUNCTION>,             \
      js::GenericCreatePrototype<cls>,                                       \
      nullptr,                                                               \
      nullptr,                                                               \
      cls##_methods,                                                         \
      cls##_properties,                                                      \
      nullptr,                                                               \
      specFlags};                                                            \
                                                                             \
  const JSClass cls::class_ = {#cls,                                         \
                               JSCLASS_HAS_RESERVED_SLOTS(nSlots) |          \
                                   JSCLASS_HAS_CACHED_PROTO(JSProto_##cls) | \
                                   classFlags,                               \
                               classOps, &cls::classSpec_};                  \
                                                                             \
  const JSClass cls::protoClass_ = {#cls ".prototype",                       \
                                    JSCLASS_HAS_CACHED_PROTO(JSProto_##cls), \
                                    JS_NULL_CLASS_OPS, &cls::classSpec_};

#endif  // builtin_streams_ClassSpecMacro_h
