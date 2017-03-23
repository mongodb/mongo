/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Symbol.h"

#include "jscntxt.h"
#include "jscompartment.h"

#include "builtin/SymbolObject.h"
#include "gc/Allocator.h"
#include "gc/Rooting.h"
#include "vm/StringBuffer.h"

#include "jscompartmentinlines.h"

using JS::Symbol;
using namespace js;

Symbol*
Symbol::newInternal(ExclusiveContext* cx, JS::SymbolCode code, JSAtom* description)
{
    MOZ_ASSERT(cx->compartment() == cx->atomsCompartment());
    MOZ_ASSERT(cx->atomsCompartment()->runtimeFromAnyThread()->currentThreadHasExclusiveAccess());

    // Following js::AtomizeString, we grudgingly forgo last-ditch GC here.
    Symbol* p = Allocate<JS::Symbol, NoGC>(cx);
    if (!p) {
        ReportOutOfMemory(cx);
        return nullptr;
    }
    return new (p) Symbol(code, description);
}

Symbol*
Symbol::new_(ExclusiveContext* cx, JS::SymbolCode code, JSString* description)
{
    RootedAtom atom(cx);
    if (description) {
        atom = AtomizeString(cx, description);
        if (!atom)
            return nullptr;
    }

    // Lock to allocate. If symbol allocation becomes a bottleneck, this can
    // probably be replaced with an assertion that we're on the main thread.
    AutoLockForExclusiveAccess lock(cx);
    AutoCompartment ac(cx, cx->atomsCompartment());
    return newInternal(cx, code, atom);
}

Symbol*
Symbol::for_(js::ExclusiveContext* cx, HandleString description)
{
    JSAtom* atom = AtomizeString(cx, description);
    if (!atom)
        return nullptr;

    AutoLockForExclusiveAccess lock(cx);

    SymbolRegistry& registry = cx->symbolRegistry();
    SymbolRegistry::AddPtr p = registry.lookupForAdd(atom);
    if (p)
        return *p;

    AutoCompartment ac(cx, cx->atomsCompartment());
    Symbol* sym = newInternal(cx, SymbolCode::InSymbolRegistry, atom);
    if (!sym)
        return nullptr;

    // p is still valid here because we have held the lock since the
    // lookupForAdd call, and newInternal can't GC.
    if (!registry.add(p, sym)) {
        // SystemAllocPolicy does not report OOM.
        ReportOutOfMemory(cx);
        return nullptr;
    }
    return sym;
}

#ifdef DEBUG
void
Symbol::dump(FILE* fp)
{
    if (isWellKnownSymbol()) {
        // All the well-known symbol names are ASCII.
        description_->dumpCharsNoNewline(fp);
    } else if (code_ == SymbolCode::InSymbolRegistry || code_ == SymbolCode::UniqueSymbol) {
        fputs(code_ == SymbolCode::InSymbolRegistry ? "Symbol.for(" : "Symbol(", fp);

        if (description_)
            description_->dumpCharsNoNewline(fp);
        else
            fputs("undefined", fp);

        fputc(')', fp);

        if (code_ == SymbolCode::UniqueSymbol)
            fprintf(fp, "@%p", (void*) this);
    } else {
        fprintf(fp, "<Invalid Symbol code=%u>", unsigned(code_));
    }
}
#endif  // DEBUG

bool
js::SymbolDescriptiveString(JSContext* cx, Symbol* sym, MutableHandleValue result)
{
    // steps 2-5
    StringBuffer sb(cx);
    if (!sb.append("Symbol("))
        return false;
    RootedString str(cx, sym->description());
    if (str) {
        if (!sb.append(str))
            return false;
    }
    if (!sb.append(')'))
        return false;

    // step 6
    str = sb.finishString();
    if (!str)
        return false;
    result.setString(str);
    return true;
}

bool
js::IsSymbolOrSymbolWrapper(Value v)
{
    return v.isSymbol() || (v.isObject() && v.toObject().is<SymbolObject>());
}

JS::Symbol*
js::ToSymbolPrimitive(Value v)
{
    MOZ_ASSERT(IsSymbolOrSymbolWrapper(v));
    return v.isSymbol() ? v.toSymbol() : v.toObject().as<SymbolObject>().unbox();
}


JS::ubi::Node::Size
JS::ubi::Concrete<JS::Symbol>::size(mozilla::MallocSizeOf mallocSizeOf) const
{
    // If we start allocating symbols in the nursery, we will need to update
    // this method.
    MOZ_ASSERT(get().isTenured());
    return js::gc::Arena::thingSize(get().asTenured().getAllocKind());
}
