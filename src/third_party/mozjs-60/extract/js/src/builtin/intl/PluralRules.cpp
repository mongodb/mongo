/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementation of the Intl.PluralRules proposal. */

#include "builtin/intl/PluralRules.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/ICUStubs.h"
#include "builtin/intl/ScopedICUObject.h"
#include "gc/FreeOp.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::AssertedCast;

using js::intl::CallICU;
using js::intl::GetAvailableLocales;
using js::intl::IcuLocale;

const ClassOps PluralRulesObject::classOps_ = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    PluralRulesObject::finalize
};

const Class PluralRulesObject::class_ = {
    js_Object_str,
    JSCLASS_HAS_RESERVED_SLOTS(PluralRulesObject::SLOT_COUNT) |
    JSCLASS_FOREGROUND_FINALIZE,
    &PluralRulesObject::classOps_
};

static bool
pluralRules_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(cx->names().PluralRules);
    return true;
}

static const JSFunctionSpec pluralRules_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf", "Intl_PluralRules_supportedLocalesOf", 1, 0),
    JS_FS_END
};

static const JSFunctionSpec pluralRules_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_PluralRules_resolvedOptions", 0, 0),
    JS_SELF_HOSTED_FN("select", "Intl_PluralRules_select", 1, 0),
    JS_FN(js_toSource_str, pluralRules_toSource, 0, 0),
    JS_FS_END
};

/**
 * PluralRules constructor.
 * Spec: ECMAScript 402 API, PluralRules, 13.2.1
 */
static bool
PluralRules(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    if (!ThrowIfNotConstructing(cx, args, "Intl.PluralRules"))
        return false;

    // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
        return false;

    if (!proto) {
        proto = GlobalObject::getOrCreatePluralRulesPrototype(cx, cx->global());
        if (!proto)
            return false;
    }

    Rooted<PluralRulesObject*> pluralRules(cx);
    pluralRules = NewObjectWithGivenProto<PluralRulesObject>(cx, proto);
    if (!pluralRules)
        return false;

    pluralRules->setReservedSlot(PluralRulesObject::INTERNALS_SLOT, NullValue());
    pluralRules->setReservedSlot(PluralRulesObject::UPLURAL_RULES_SLOT, PrivateValue(nullptr));
    pluralRules->setReservedSlot(PluralRulesObject::UNUMBER_FORMAT_SLOT, PrivateValue(nullptr));

    HandleValue locales = args.get(0);
    HandleValue options = args.get(1);

    // Step 3.
    if (!intl::InitializeObject(cx, pluralRules, cx->names().InitializePluralRules, locales,
                                options))
    {
        return false;
    }

    args.rval().setObject(*pluralRules);
    return true;
}

void
js::PluralRulesObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->onActiveCooperatingThread());

    PluralRulesObject* pluralRules = &obj->as<PluralRulesObject>();

    const Value& prslot = pluralRules->getReservedSlot(PluralRulesObject::UPLURAL_RULES_SLOT);
    UPluralRules* pr = static_cast<UPluralRules*>(prslot.toPrivate());

    const Value& nfslot = pluralRules->getReservedSlot(PluralRulesObject::UNUMBER_FORMAT_SLOT);
    UNumberFormat* nf = static_cast<UNumberFormat*>(nfslot.toPrivate());

    if (pr)
        uplrules_close(pr);

    if (nf)
        unum_close(nf);
}

JSObject*
js::CreatePluralRulesPrototype(JSContext* cx, HandleObject Intl, Handle<GlobalObject*> global)
{
    RootedFunction ctor(cx);
    ctor = global->createConstructor(cx, &PluralRules, cx->names().PluralRules, 0);
    if (!ctor)
        return nullptr;

    RootedObject proto(cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
    if (!proto)
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return nullptr;

    if (!JS_DefineFunctions(cx, ctor, pluralRules_static_methods))
        return nullptr;

    if (!JS_DefineFunctions(cx, proto, pluralRules_methods))
        return nullptr;

    RootedValue ctorValue(cx, ObjectValue(*ctor));
    if (!DefineDataProperty(cx, Intl, cx->names().PluralRules, ctorValue, 0))
        return nullptr;

    return proto;
}

bool
js::intl_PluralRules_availableLocales(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    RootedValue result(cx);
    // We're going to use ULocale availableLocales as per ICU recommendation:
    // https://ssl.icu-project.org/trac/ticket/12756
    if (!GetAvailableLocales(cx, uloc_countAvailable, uloc_getAvailable, &result))
        return false;
    args.rval().set(result);
    return true;
}

/**
 * This creates new UNumberFormat with calculated digit formatting
 * properties for PluralRules.
 *
 * This is similar to NewUNumberFormat but doesn't allow for currency or
 * percent types.
 */
static UNumberFormat*
NewUNumberFormatForPluralRules(JSContext* cx, Handle<PluralRulesObject*> pluralRules)
{
    RootedObject internals(cx, intl::GetInternalsObject(cx, pluralRules));
    if (!internals)
       return nullptr;

    RootedValue value(cx);

    if (!GetProperty(cx, internals, internals, cx->names().locale, &value))
        return nullptr;
    JSAutoByteString locale(cx, value.toString());
    if (!locale)
        return nullptr;

    uint32_t uMinimumIntegerDigits = 1;
    uint32_t uMinimumFractionDigits = 0;
    uint32_t uMaximumFractionDigits = 3;
    int32_t uMinimumSignificantDigits = -1;
    int32_t uMaximumSignificantDigits = -1;

    bool hasP;
    if (!HasProperty(cx, internals, cx->names().minimumSignificantDigits, &hasP))
        return nullptr;

    if (hasP) {
        if (!GetProperty(cx, internals, internals, cx->names().minimumSignificantDigits, &value))
            return nullptr;
        uMinimumSignificantDigits = value.toInt32();

        if (!GetProperty(cx, internals, internals, cx->names().maximumSignificantDigits, &value))
            return nullptr;
        uMaximumSignificantDigits = value.toInt32();
    } else {
        if (!GetProperty(cx, internals, internals, cx->names().minimumIntegerDigits, &value))
            return nullptr;
        uMinimumIntegerDigits = AssertedCast<uint32_t>(value.toInt32());

        if (!GetProperty(cx, internals, internals, cx->names().minimumFractionDigits, &value))
            return nullptr;
        uMinimumFractionDigits = AssertedCast<uint32_t>(value.toInt32());

        if (!GetProperty(cx, internals, internals, cx->names().maximumFractionDigits, &value))
            return nullptr;
        uMaximumFractionDigits = AssertedCast<uint32_t>(value.toInt32());
    }

    UErrorCode status = U_ZERO_ERROR;
    UNumberFormat* nf =
        unum_open(UNUM_DECIMAL, nullptr, 0, IcuLocale(locale.ptr()), nullptr, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return nullptr;
    }
    ScopedICUObject<UNumberFormat, unum_close> toClose(nf);

    if (uMinimumSignificantDigits != -1) {
        unum_setAttribute(nf, UNUM_SIGNIFICANT_DIGITS_USED, true);
        unum_setAttribute(nf, UNUM_MIN_SIGNIFICANT_DIGITS, uMinimumSignificantDigits);
        unum_setAttribute(nf, UNUM_MAX_SIGNIFICANT_DIGITS, uMaximumSignificantDigits);
    } else {
        unum_setAttribute(nf, UNUM_MIN_INTEGER_DIGITS, uMinimumIntegerDigits);
        unum_setAttribute(nf, UNUM_MIN_FRACTION_DIGITS, uMinimumFractionDigits);
        unum_setAttribute(nf, UNUM_MAX_FRACTION_DIGITS, uMaximumFractionDigits);
    }

    return toClose.forget();
}

/**
 * Returns a new UPluralRules with the locale and type options of the given
 * PluralRules.
 */
static UPluralRules*
NewUPluralRules(JSContext* cx, Handle<PluralRulesObject*> pluralRules)
{
    RootedObject internals(cx, intl::GetInternalsObject(cx, pluralRules));
    if (!internals)
        return nullptr;

    RootedValue value(cx);

    if (!GetProperty(cx, internals, internals, cx->names().locale, &value))
        return nullptr;
    JSAutoByteString locale(cx, value.toString());
    if (!locale)
        return nullptr;

    if (!GetProperty(cx, internals, internals, cx->names().type, &value))
        return nullptr;

    UPluralType category;
    {
        JSLinearString* type = value.toString()->ensureLinear(cx);
        if (!type)
            return nullptr;

        if (StringEqualsAscii(type, "cardinal")) {
            category = UPLURAL_TYPE_CARDINAL;
        } else {
            MOZ_ASSERT(StringEqualsAscii(type, "ordinal"));
            category = UPLURAL_TYPE_ORDINAL;
        }
    }

    UErrorCode status = U_ZERO_ERROR;
    UPluralRules* pr = uplrules_openForType(IcuLocale(locale.ptr()), category, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return nullptr;
    }
    return pr;
}

bool
js::intl_SelectPluralRule(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);

    Rooted<PluralRulesObject*> pluralRules(cx, &args[0].toObject().as<PluralRulesObject>());

    double x = args[1].toNumber();

    // Obtain a cached UPluralRules object.
    void* priv = pluralRules->getReservedSlot(PluralRulesObject::UPLURAL_RULES_SLOT).toPrivate();
    UPluralRules* pr = static_cast<UPluralRules*>(priv);
    if (!pr) {
        pr = NewUPluralRules(cx, pluralRules);
        if (!pr)
            return false;
        pluralRules->setReservedSlot(PluralRulesObject::UPLURAL_RULES_SLOT, PrivateValue(pr));
    }

    // Obtain a cached UNumberFormat object.
    priv = pluralRules->getReservedSlot(PluralRulesObject::UNUMBER_FORMAT_SLOT).toPrivate();
    UNumberFormat* nf = static_cast<UNumberFormat*>(priv);
    if (!nf) {
        nf = NewUNumberFormatForPluralRules(cx, pluralRules);
        if (!nf)
            return false;
        pluralRules->setReservedSlot(PluralRulesObject::UNUMBER_FORMAT_SLOT, PrivateValue(nf));
    }

    JSString* str = CallICU(cx, [pr, x, nf](UChar* chars, int32_t size, UErrorCode* status) {
        return uplrules_selectWithFormat(pr, x, nf, chars, size, status);
    });
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

bool
js::intl_GetPluralCategories(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    Rooted<PluralRulesObject*> pluralRules(cx, &args[0].toObject().as<PluralRulesObject>());

    // Obtain a cached UPluralRules object.
    void* priv = pluralRules->getReservedSlot(PluralRulesObject::UPLURAL_RULES_SLOT).toPrivate();
    UPluralRules* pr = static_cast<UPluralRules*>(priv);
    if (!pr) {
        pr = NewUPluralRules(cx, pluralRules);
        if (!pr)
            return false;
        pluralRules->setReservedSlot(PluralRulesObject::UPLURAL_RULES_SLOT, PrivateValue(pr));
    }

    UErrorCode status = U_ZERO_ERROR;
    UEnumeration* ue = uplrules_getKeywords(pr, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UEnumeration, uenum_close> closeEnum(ue);

    RootedObject res(cx, NewDenseEmptyArray(cx));
    if (!res)
        return false;

    RootedValue element(cx);
    uint32_t i = 0;

    do {
        int32_t catSize;
        const char* cat = uenum_next(ue, &catSize, &status);
        if (U_FAILURE(status)) {
            intl::ReportInternalError(cx);
            return false;
        }

        if (!cat)
            break;

        MOZ_ASSERT(catSize >= 0);
        JSString* str = NewStringCopyN<CanGC>(cx, cat, catSize);
        if (!str)
            return false;

        element.setString(str);
        if (!DefineDataElement(cx, res, i++, element))
            return false;
    } while (true);

    args.rval().setObject(*res);
    return true;
}
