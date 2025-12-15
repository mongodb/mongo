/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Interfaces by which the embedding can interact with the Debugger API.

#ifndef js_Debug_h
#define js_Debug_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/BaseProfilerUtils.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Vector.h"

#include <utility>

#include "jstypes.h"

#include "js/GCAPI.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

namespace js {
class Debugger;
}  // namespace js

/* Defined in vm/Debugger.cpp. */
extern JS_PUBLIC_API bool JS_DefineDebuggerObject(JSContext* cx,
                                                  JS::HandleObject obj);

// If the JS execution tracer is running, this will generate a
// ENTRY_KIND_LABEL_ENTER entry with the specified label.
// The consumer of the trace can then, for instance, correlate all code running
// after this entry and before the corresponding ENTRY_KIND_LABEL_LEAVE with the
// provided label.
// If the tracer is not running, this does nothing.
extern JS_PUBLIC_API void JS_TracerEnterLabelLatin1(JSContext* cx,
                                                    const char* label);
extern JS_PUBLIC_API void JS_TracerEnterLabelTwoByte(JSContext* cx,
                                                     const char16_t* label);

extern JS_PUBLIC_API bool JS_TracerIsTracing(JSContext* cx);

// If the JS execution tracer is running, this will generate a
// ENTRY_KIND_LABEL_LEAVE entry with the specified label.
// It is up to the consumer to decide what to do with a ENTRY_KIND_LABEL_LEAVE
// entry is encountered without a corresponding ENTRY_KIND_LABEL_ENTER.
// If the tracer is not running, this does nothing.
extern JS_PUBLIC_API void JS_TracerLeaveLabelLatin1(JSContext* cx,
                                                    const char* label);
extern JS_PUBLIC_API void JS_TracerLeaveLabelTwoByte(JSContext* cx,
                                                     const char16_t* label);

#ifdef MOZ_EXECUTION_TRACING

// This will begin execution tracing for the JSContext, i.e., this will begin
// recording every entrance into / exit from a function for the given context.
// The trace can be read via JS_TracerSnapshotTrace, and populates the
// ExecutionTrace struct defined below.
//
// This throws if the code coverage is active for any realm in the context.
extern JS_PUBLIC_API bool JS_TracerBeginTracing(JSContext* cx);

// This ends execution tracing for the JSContext, discards the tracing
// buffers, and clears some caches used for tracing. JS_TracerSnapshotTrace
// should be called *before* JS_TracerEndTracing if you want to read the trace
// data for this JSContext.
extern JS_PUBLIC_API bool JS_TracerEndTracing(JSContext* cx);

namespace JS {

// This is populated by JS_TracerSnapshotTrace and just represent a minimal
// structure for natively representing an execution trace across a range of
// JSContexts (see below). The core of the trace is an array of events, each of
// which is a tagged union with data corresponding to that event. Events can
// also point into various tables, and store all of their string data in a
// contiguous UTF-8 stringBuffer (each string is null-terminated within the
// buffer.)
struct ExecutionTrace {
  enum class EventKind : uint8_t {
    FunctionEnter = 0,
    FunctionLeave = 1,
    LabelEnter = 2,
    LabelLeave = 3,

    // NOTE: the `Error` event has no TracedEvent payload, and will always
    // represent the end of the trace when encountered.
    Error = 4,
  };

  enum class ImplementationType : uint8_t {
    Interpreter = 0,
    Baseline = 1,
    Ion = 2,
    Wasm = 3,
  };

  struct TracedEvent {
    EventKind kind;
    union {
      // For FunctionEnter / FunctionLeave
      struct {
        ImplementationType implementation;

        // 1-origin line number of the function
        uint32_t lineNumber;

        // 1-origin column of the function
        uint32_t column;

        // Keys into the thread's scriptUrls HashMap. This key can be missing
        // from the HashMap, although ideally that situation is rare (it is
        // more likely in long running traces with *many* unique functions
        // and/or scripts)
        uint32_t scriptId;

        // ID to the realm that the frame was in. It's used for finding which
        // frame comes from which window/page.
        uint64_t realmID;

        // Keys into the thread's atoms HashMap. This key can be missing from
        // the HashMap as well (see comment above scriptId)
        uint32_t functionNameId;
      } functionEvent;

      // For LabelEnter / LabelLeave
      struct {
        size_t label;  // Indexes directly into the trace's stringBuffer
      } labelEvent;
    };
    // Milliseconds since process creation
    double time;
  };

  struct TracedJSContext {
    mozilla::baseprofiler::BaseProfilerThreadId id;

    // Maps ids to indices into the trace's stringBuffer
    mozilla::HashMap<uint32_t, size_t> scriptUrls;

    // Similar to scriptUrls
    mozilla::HashMap<uint32_t, size_t> atoms;

    mozilla::Vector<TracedEvent> events;
  };

  mozilla::Vector<char> stringBuffer;

  // This will be populated with an entry for each context which had tracing
  // enabled via JS_TracerBeginTracing.
  mozilla::Vector<TracedJSContext> contexts;
};
}  // namespace JS

// Captures the trace for all JSContexts in the process which are currently
// tracing.
extern JS_PUBLIC_API bool JS_TracerSnapshotTrace(JS::ExecutionTrace& trace);

#endif /* MOZ_EXECUTION_TRACING */

namespace JS {
namespace dbg {

// [SMDOC] Debugger builder API
//
// Helping embedding code build objects for Debugger
// -------------------------------------------------
//
// Some Debugger API features lean on the embedding application to construct
// their result values. For example, Debugger.Frame.prototype.scriptEntryReason
// calls hooks provided by the embedding to construct values explaining why it
// invoked JavaScript; if F is a frame called from a mouse click event handler,
// F.scriptEntryReason would return an object of the form:
//
//   { eventType: "mousedown", event: <object> }
//
// where <object> is a Debugger.Object whose referent is the event being
// dispatched.
//
// However, Debugger implements a trust boundary. Debuggee code may be
// considered untrusted; debugger code needs to be protected from debuggee
// getters, setters, proxies, Object.watch watchpoints, and any other feature
// that might accidentally cause debugger code to set the debuggee running. The
// Debugger API tries to make it easy to write safe debugger code by only
// offering access to debuggee objects via Debugger.Object instances, which
// ensure that only those operations whose explicit purpose is to invoke
// debuggee code do so. But this protective membrane is only helpful if we
// interpose Debugger.Object instances in all the necessary spots.
//
// SpiderMonkey's compartment system also implements a trust boundary. The
// debuggee and debugger are always in different compartments. Inter-compartment
// work requires carefully tracking which compartment each JSObject or JS::Value
// belongs to, and ensuring that is is correctly wrapped for each operation.
//
// It seems precarious to expect the embedding's hooks to implement these trust
// boundaries. Instead, the JS::dbg::Builder API segregates the code which
// constructs trusted objects from that which deals with untrusted objects.
// Trusted objects have an entirely different C++ type, so code that improperly
// mixes trusted and untrusted objects is caught at compile time.
//
// In the structure shown above, there are two trusted objects, and one
// untrusted object:
//
// - The overall object, with the 'eventType' and 'event' properties, is a
//   trusted object. We're going to return it to D.F.p.scriptEntryReason's
//   caller, which will handle it directly.
//
// - The Debugger.Object instance appearing as the value of the 'event' property
//   is a trusted object. It belongs to the same Debugger instance as the
//   Debugger.Frame instance whose scriptEntryReason accessor was called, and
//   presents a safe reflection-oriented API for inspecting its referent, which
//   is:
//
// - The actual event object, an untrusted object, and the referent of the
//   Debugger.Object above. (Content can do things like replacing accessors on
//   Event.prototype.)
//
// Using JS::dbg::Builder, all objects and values the embedding deals with
// directly are considered untrusted, and are assumed to be debuggee values. The
// only way to construct trusted objects is to use Builder's own methods, which
// return a separate Object type. The only way to set a property on a trusted
// object is through that Object type. The actual trusted object is never
// exposed to the embedding.
//
// So, for example, the embedding might use code like the following to construct
// the object shown above, given a Builder passed to it by Debugger:
//
//    bool
//    MyScriptEntryReason::explain(JSContext* cx,
//                                 Builder& builder,
//                                 Builder::Object& result)
//    {
//        JSObject* eventObject = ... obtain debuggee event object somehow ...;
//        if (!eventObject) {
//            return false;
//        }
//        result = builder.newObject(cx);
//        return result &&
//               result.defineProperty(cx, "eventType",
//                                     SafelyFetchType(eventObject)) &&
//               result.defineProperty(cx, "event", eventObject);
//    }
//
//
// Object::defineProperty also accepts an Object as the value to store on the
// property. By its type, we know that the value is trusted, so we set it
// directly as the property's value, without interposing a Debugger.Object
// wrapper. This allows the embedding to builted nested structures of trusted
// objects.
//
// The Builder and Builder::Object methods take care of doing whatever
// compartment switching and wrapping are necessary to construct the trusted
// values in the Debugger's compartment.
//
// The Object type is self-rooting. Construction, assignment, and destruction
// all properly root the referent object.

class BuilderOrigin;

class Builder {
  // The Debugger instance whose client we are building a value for. We build
  // objects in this object's compartment.
  PersistentRootedObject debuggerObject;

  // debuggerObject's Debugger structure, for convenience.
  js::Debugger* debugger;

  // Check that |thing| is in the same compartment as our debuggerObject. Used
  // for assertions when constructing BuiltThings. We can overload this as we
  // add more instantiations of BuiltThing.
#ifdef DEBUG
  void assertBuilt(JSObject* obj);
#else
  void assertBuilt(JSObject* obj) {}
#endif

 protected:
  // A reference to a trusted object or value. At the moment, we only use it
  // with JSObject*.
  template <typename T>
  class BuiltThing {
    friend class BuilderOrigin;

   protected:
    // The Builder to which this trusted thing belongs.
    Builder& owner;

    // A rooted reference to our value.
    PersistentRooted<T> value;

    BuiltThing(JSContext* cx, Builder& owner_,
               T value_ = SafelyInitialized<T>::create())
        : owner(owner_), value(cx, value_) {
      owner.assertBuilt(value_);
    }

    // Forward some things from our owner, for convenience.
    js::Debugger* debugger() const { return owner.debugger; }
    JSObject* debuggerObject() const { return owner.debuggerObject; }

   public:
    BuiltThing(const BuiltThing& rhs) : owner(rhs.owner), value(rhs.value) {}
    BuiltThing& operator=(const BuiltThing& rhs) {
      MOZ_ASSERT(&owner == &rhs.owner);
      owner.assertBuilt(rhs.value);
      value = rhs.value;
      return *this;
    }

    explicit operator bool() const {
      // If we ever instantiate BuiltThing<Value>, this might not suffice.
      return value;
    }

   private:
    BuiltThing() = delete;
  };

 public:
  // A reference to a trusted object, possibly null. Instances of Object are
  // always properly rooted. They can be copied and assigned, as if they were
  // pointers.
  class Object : private BuiltThing<JSObject*> {
    friend class Builder;        // for construction
    friend class BuilderOrigin;  // for unwrapping

    typedef BuiltThing<JSObject*> Base;

    // This is private, because only Builders can create Objects that
    // actually point to something (hence the 'friend' declaration).
    Object(JSContext* cx, Builder& owner_, HandleObject obj)
        : Base(cx, owner_, obj.get()) {}

    bool definePropertyToTrusted(JSContext* cx, const char* name,
                                 JS::MutableHandleValue value);

   public:
    Object(JSContext* cx, Builder& owner_) : Base(cx, owner_, nullptr) {}
    Object(const Object& rhs) = default;

    // Our automatically-generated assignment operator can see our base
    // class's assignment operator, so we don't need to write one out here.

    // Set the property named |name| on this object to |value|.
    //
    // If |value| is a string or primitive, re-wrap it for the debugger's
    // compartment.
    //
    // If |value| is an object, assume it is a debuggee object and make a
    // Debugger.Object instance referring to it. Set that as the propery's
    // value.
    //
    // If |value| is another trusted object, store it directly as the
    // property's value.
    //
    // On error, report the problem on cx and return false.
    bool defineProperty(JSContext* cx, const char* name, JS::HandleValue value);
    bool defineProperty(JSContext* cx, const char* name,
                        JS::HandleObject value);
    bool defineProperty(JSContext* cx, const char* name, Object& value);

    using Base::operator bool;
  };

  // Build an empty object for direct use by debugger code, owned by this
  // Builder. If an error occurs, report it on cx and return a false Object.
  Object newObject(JSContext* cx);

 protected:
  Builder(JSContext* cx, js::Debugger* debugger);
};

// Debugger itself instantiates this subclass of Builder, which can unwrap
// BuiltThings that belong to it.
class BuilderOrigin : public Builder {
  template <typename T>
  T unwrapAny(const BuiltThing<T>& thing) {
    MOZ_ASSERT(&thing.owner == this);
    return thing.value.get();
  }

 public:
  BuilderOrigin(JSContext* cx, js::Debugger* debugger_)
      : Builder(cx, debugger_) {}

  JSObject* unwrap(Object& object) { return unwrapAny(object); }
};

// Finding the size of blocks allocated with malloc
// ------------------------------------------------
//
// Debugger.Memory wants to be able to report how many bytes items in memory are
// consuming. To do this, it needs a function that accepts a pointer to a block,
// and returns the number of bytes allocated to that block. SpiderMonkey itself
// doesn't know which function is appropriate to use, but the embedding does.

// Tell Debuggers in |cx| to use |mallocSizeOf| to find the size of
// malloc'd blocks.
JS_PUBLIC_API void SetDebuggerMallocSizeOf(JSContext* cx,
                                           mozilla::MallocSizeOf mallocSizeOf);

// Get the MallocSizeOf function that the given context is using to find the
// size of malloc'd blocks.
JS_PUBLIC_API mozilla::MallocSizeOf GetDebuggerMallocSizeOf(JSContext* cx);

// Debugger and Garbage Collection Events
// --------------------------------------
//
// The Debugger wants to report about its debuggees' GC cycles, however entering
// JS after a GC is troublesome since SpiderMonkey will often do something like
// force a GC and then rely on the nursery being empty. If we call into some
// Debugger's hook after the GC, then JS runs and the nursery won't be
// empty. Instead, we rely on embedders to call back into SpiderMonkey after a
// GC and notify Debuggers to call their onGarbageCollection hook.

// Determine whether it's necessary to call FireOnGarbageCollectionHook() after
// a GC. This is only required if there are debuggers with an
// onGarbageCollection hook observing a global in the set of collected zones.
JS_PUBLIC_API bool FireOnGarbageCollectionHookRequired(JSContext* cx);

// For each Debugger that observed a debuggee involved in the given GC event,
// call its `onGarbageCollection` hook.
JS_PUBLIC_API bool FireOnGarbageCollectionHook(
    JSContext* cx, GarbageCollectionEvent::Ptr&& data);

// Return true if the given value is a Debugger object, false otherwise.
JS_PUBLIC_API bool IsDebugger(JSObject& obj);

// Append each of the debuggee global objects observed by the Debugger object
// |dbgObj| to |vector|. Returns true on success, false on failure.
JS_PUBLIC_API bool GetDebuggeeGlobals(JSContext* cx, JSObject& dbgObj,
                                      MutableHandleObjectVector vector);

// Returns true if there's any debugger attached to the given context where
// the debugger's "shouldAvoidSideEffects" property is true.
//
// This is supposed to be used by native code that performs side-effectful
// operations where the debugger cannot hook it.
//
// If this function returns true, the native function should throw an
// uncatchable exception by returning `false` without setting any pending
// exception. The debugger will handle this exception by aborting the eager
// evaluation.
//
// The native code can opt into this behavior to help the debugger performing
// the side-effect-free evaluation.
//
// Expected consumers of this API include JSClassOps.resolve hooks which have
// any side-effect other than just resolving the property.
//
// Example:
//   static bool ResolveHook(JSContext* cx, HandleObject obj, HandleId id,
//                           bool* resolvedp) {
//     *resolvedp = false;
//     if (JS::dbg::ShouldAvoidSideEffects()) {
//       return false;
//     }
//     // Resolve the property with the side-effect.
//     ...
//     return true;
//   }
bool ShouldAvoidSideEffects(JSContext* cx);

}  // namespace dbg
}  // namespace JS

#endif /* js_Debug_h */
