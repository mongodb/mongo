// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "DBRef" Javascript object.
 *
 * These look like:
 * {
 *     $ref : String(),
 *     $id : Any,
 *     $db : String(),
 * }
 */
struct [[MONGO_MOD_PUBLIC]] DBRefInfo : public BaseInfo {
    enum Slots { BSONHolderSlot, DBRefInfoSlotCount };

    static void construct(JSContext* cx, JS::CallArgs args);

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(DBRefInfoSlotCount) | BaseInfo::finalizeFlag;

    static void delProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::ObjectOpResult& result);
    static void enumerate(JSContext* cx,
                          JS::HandleObject obj,
                          JS::MutableHandleIdVector properties,
                          bool enumerableOnly);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);
    static void resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp);
    static void setProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::HandleValue vp,
                            JS::HandleValue receiver,
                            JS::ObjectOpResult& result);

    static void make(
        JSContext* cx, JS::MutableHandleObject obj, BSONObj bson, const BSONObj* parent, bool ro);
};

}  // namespace mozjs
}  // namespace mongo
