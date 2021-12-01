/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.NumberFormat implementation. */

#include "builtin/intl/NumberFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/FloatingPoint.h"

#include <algorithm>
#include <stddef.h>
#include <stdint.h>

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/ICUStubs.h"
#include "builtin/intl/ScopedICUObject.h"
#include "ds/Sort.h"
#include "gc/FreeOp.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "vm/JSContext.h"
#include "vm/SelfHosting.h"
#include "vm/Stack.h"

#include "vm/JSObject-inl.h"

using namespace js;

using mozilla::AssertedCast;
using mozilla::IsFinite;
using mozilla::IsNaN;
using mozilla::IsNegativeZero;

using js::intl::CallICU;
using js::intl::DateTimeFormatOptions;
using js::intl::GetAvailableLocales;
using js::intl::IcuLocale;

const ClassOps NumberFormatObject::classOps_ = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    NumberFormatObject::finalize
};

const Class NumberFormatObject::class_ = {
    js_Object_str,
    JSCLASS_HAS_RESERVED_SLOTS(NumberFormatObject::SLOT_COUNT) |
    JSCLASS_FOREGROUND_FINALIZE,
    &NumberFormatObject::classOps_
};

static bool
numberFormat_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(cx->names().NumberFormat);
    return true;
}

static const JSFunctionSpec numberFormat_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf", "Intl_NumberFormat_supportedLocalesOf", 1, 0),
    JS_FS_END
};

static const JSFunctionSpec numberFormat_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_NumberFormat_resolvedOptions", 0, 0),
    JS_SELF_HOSTED_FN("formatToParts", "Intl_NumberFormat_formatToParts", 1, 0),
    JS_FN(js_toSource_str, numberFormat_toSource, 0, 0),
    JS_FS_END
};

static const JSPropertySpec numberFormat_properties[] = {
    JS_SELF_HOSTED_GET("format", "Intl_NumberFormat_format_get", 0),
    JS_STRING_SYM_PS(toStringTag, "Object", JSPROP_READONLY),
    JS_PS_END
};

/**
 * 11.2.1 Intl.NumberFormat([ locales [, options]])
 *
 * ES2017 Intl draft rev 94045d234762ad107a3d09bb6f7381a65f1a2f9b
 */
static bool
NumberFormat(JSContext* cx, const CallArgs& args, bool construct)
{
    // Step 1 (Handled by OrdinaryCreateFromConstructor fallback code).

    // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
        return false;

    if (!proto) {
        proto = GlobalObject::getOrCreateNumberFormatPrototype(cx, cx->global());
        if (!proto)
            return false;
    }

    Rooted<NumberFormatObject*> numberFormat(cx);
    numberFormat = NewObjectWithGivenProto<NumberFormatObject>(cx, proto);
    if (!numberFormat)
        return false;

    numberFormat->setReservedSlot(NumberFormatObject::INTERNALS_SLOT, NullValue());
    numberFormat->setReservedSlot(NumberFormatObject::UNUMBER_FORMAT_SLOT, PrivateValue(nullptr));

    RootedValue thisValue(cx, construct ? ObjectValue(*numberFormat) : args.thisv());
    HandleValue locales = args.get(0);
    HandleValue options = args.get(1);

    // Step 3.
    return intl::LegacyInitializeObject(cx, numberFormat, cx->names().InitializeNumberFormat,
                                        thisValue, locales, options,
                                        DateTimeFormatOptions::Standard, args.rval());
}

static bool
NumberFormat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return NumberFormat(cx, args, args.isConstructing());
}

bool
js::intl_NumberFormat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(!args.isConstructing());
    // intl_NumberFormat is an intrinsic for self-hosted JavaScript, so it
    // cannot be used with "new", but it still has to be treated as a
    // constructor.
    return NumberFormat(cx, args, true);
}

void
js::NumberFormatObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->onActiveCooperatingThread());

    const Value& slot =
        obj->as<NumberFormatObject>().getReservedSlot(NumberFormatObject::UNUMBER_FORMAT_SLOT);
    if (UNumberFormat* nf = static_cast<UNumberFormat*>(slot.toPrivate()))
        unum_close(nf);
}

JSObject*
js::CreateNumberFormatPrototype(JSContext* cx, HandleObject Intl, Handle<GlobalObject*> global,
                                MutableHandleObject constructor)
{
    RootedFunction ctor(cx);
    ctor = GlobalObject::createConstructor(cx, &NumberFormat, cx->names().NumberFormat, 0);
    if (!ctor)
        return nullptr;

    RootedObject proto(cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
    if (!proto)
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return nullptr;

    // 11.3.2
    if (!JS_DefineFunctions(cx, ctor, numberFormat_static_methods))
        return nullptr;

    // 11.4.4
    if (!JS_DefineFunctions(cx, proto, numberFormat_methods))
        return nullptr;

    // 11.4.2 and 11.4.3
    if (!JS_DefineProperties(cx, proto, numberFormat_properties))
        return nullptr;

    // 8.1
    RootedValue ctorValue(cx, ObjectValue(*ctor));
    if (!DefineDataProperty(cx, Intl, cx->names().NumberFormat, ctorValue, 0))
        return nullptr;

    constructor.set(ctor);
    return proto;
}

bool
js::intl_NumberFormat_availableLocales(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    RootedValue result(cx);
    if (!GetAvailableLocales(cx, unum_countAvailable, unum_getAvailable, &result))
        return false;
    args.rval().set(result);
    return true;
}

bool
js::intl_numberingSystem(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isString());

    JSAutoByteString locale(cx, args[0].toString());
    if (!locale)
        return false;

    UErrorCode status = U_ZERO_ERROR;
    UNumberingSystem* numbers = unumsys_open(IcuLocale(locale.ptr()), &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }

    ScopedICUObject<UNumberingSystem, unumsys_close> toClose(numbers);

    const char* name = unumsys_getName(numbers);
    JSString* jsname = JS_NewStringCopyZ(cx, name);
    if (!jsname)
        return false;

    args.rval().setString(jsname);
    return true;
}

/**
 * Returns a new UNumberFormat with the locale and number formatting options
 * of the given NumberFormat.
 */
static UNumberFormat*
NewUNumberFormat(JSContext* cx, Handle<NumberFormatObject*> numberFormat)
{
    RootedValue value(cx);

    RootedObject internals(cx, intl::GetInternalsObject(cx, numberFormat));
    if (!internals)
       return nullptr;

    if (!GetProperty(cx, internals, internals, cx->names().locale, &value))
        return nullptr;
    JSAutoByteString locale(cx, value.toString());
    if (!locale)
        return nullptr;

    // UNumberFormat options with default values
    UNumberFormatStyle uStyle = UNUM_DECIMAL;
    const UChar* uCurrency = nullptr;
    uint32_t uMinimumIntegerDigits = 1;
    uint32_t uMinimumFractionDigits = 0;
    uint32_t uMaximumFractionDigits = 3;
    int32_t uMinimumSignificantDigits = -1;
    int32_t uMaximumSignificantDigits = -1;
    bool uUseGrouping = true;

    // Sprinkle appropriate rooting flavor over things the GC might care about.
    RootedString currency(cx);
    AutoStableStringChars stableChars(cx);

    // We don't need to look at numberingSystem - it can only be set via
    // the Unicode locale extension and is therefore already set on locale.

    if (!GetProperty(cx, internals, internals, cx->names().style, &value))
        return nullptr;

    {
        JSLinearString* style = value.toString()->ensureLinear(cx);
        if (!style)
            return nullptr;

        if (StringEqualsAscii(style, "currency")) {
            if (!GetProperty(cx, internals, internals, cx->names().currency, &value))
                return nullptr;
            currency = value.toString();
            MOZ_ASSERT(currency->length() == 3,
                       "IsWellFormedCurrencyCode permits only length-3 strings");
            if (!stableChars.initTwoByte(cx, currency))
                return nullptr;
            // uCurrency remains owned by stableChars.
            uCurrency = stableChars.twoByteRange().begin().get();

            if (!GetProperty(cx, internals, internals, cx->names().currencyDisplay, &value))
                return nullptr;
            JSLinearString* currencyDisplay = value.toString()->ensureLinear(cx);
            if (!currencyDisplay)
                return nullptr;
            if (StringEqualsAscii(currencyDisplay, "code")) {
                uStyle = UNUM_CURRENCY_ISO;
            } else if (StringEqualsAscii(currencyDisplay, "symbol")) {
                uStyle = UNUM_CURRENCY;
            } else {
                MOZ_ASSERT(StringEqualsAscii(currencyDisplay, "name"));
                uStyle = UNUM_CURRENCY_PLURAL;
            }
        } else if (StringEqualsAscii(style, "percent")) {
            uStyle = UNUM_PERCENT;
        } else {
            MOZ_ASSERT(StringEqualsAscii(style, "decimal"));
            uStyle = UNUM_DECIMAL;
        }
    }

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

    if (!GetProperty(cx, internals, internals, cx->names().useGrouping, &value))
        return nullptr;
    uUseGrouping = value.toBoolean();

    UErrorCode status = U_ZERO_ERROR;
    UNumberFormat* nf = unum_open(uStyle, nullptr, 0, IcuLocale(locale.ptr()), nullptr, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return nullptr;
    }
    ScopedICUObject<UNumberFormat, unum_close> toClose(nf);

    if (uCurrency) {
        unum_setTextAttribute(nf, UNUM_CURRENCY_CODE, uCurrency, 3, &status);
        if (U_FAILURE(status)) {
            intl::ReportInternalError(cx);
            return nullptr;
        }
    }
    if (uMinimumSignificantDigits != -1) {
        unum_setAttribute(nf, UNUM_SIGNIFICANT_DIGITS_USED, true);
        unum_setAttribute(nf, UNUM_MIN_SIGNIFICANT_DIGITS, uMinimumSignificantDigits);
        unum_setAttribute(nf, UNUM_MAX_SIGNIFICANT_DIGITS, uMaximumSignificantDigits);
    } else {
        unum_setAttribute(nf, UNUM_MIN_INTEGER_DIGITS, uMinimumIntegerDigits);
        unum_setAttribute(nf, UNUM_MIN_FRACTION_DIGITS, uMinimumFractionDigits);
        unum_setAttribute(nf, UNUM_MAX_FRACTION_DIGITS, uMaximumFractionDigits);
    }
    unum_setAttribute(nf, UNUM_GROUPING_USED, uUseGrouping);
    unum_setAttribute(nf, UNUM_ROUNDING_MODE, UNUM_ROUND_HALFUP);

    return toClose.forget();
}

static JSString*
PartitionNumberPattern(JSContext* cx, UNumberFormat* nf, double* x,
                       UFieldPositionIterator* fpositer)
{
    // PartitionNumberPattern doesn't consider -0.0 to be negative.
    if (IsNegativeZero(*x))
        *x = 0.0;

    return CallICU(cx, [nf, x, fpositer](UChar* chars, int32_t size, UErrorCode* status) {
        return unum_formatDoubleForFields(nf, *x, chars, size, fpositer, status);
    });
}

static bool
intl_FormatNumber(JSContext* cx, UNumberFormat* nf, double x, MutableHandleValue result)
{
    // Passing null for |fpositer| will just not compute partition information,
    // letting us common up all ICU number-formatting code.
    JSString* str = PartitionNumberPattern(cx, nf, &x, nullptr);
    if (!str)
        return false;

    result.setString(str);
    return true;
}

using FieldType = ImmutablePropertyNamePtr JSAtomState::*;

static FieldType
GetFieldTypeForNumberField(UNumberFormatFields fieldName, double d)
{
    // See intl/icu/source/i18n/unicode/unum.h for a detailed field list.  This
    // list is deliberately exhaustive: cases might have to be added/removed if
    // this code is compiled with a different ICU with more UNumberFormatFields
    // enum initializers.  Please guard such cases with appropriate ICU
    // version-testing #ifdefs, should cross-version divergence occur.
    switch (fieldName) {
      case UNUM_INTEGER_FIELD:
        if (IsNaN(d))
            return &JSAtomState::nan;
        if (!IsFinite(d))
            return &JSAtomState::infinity;
        return &JSAtomState::integer;

      case UNUM_GROUPING_SEPARATOR_FIELD:
        return &JSAtomState::group;

      case UNUM_DECIMAL_SEPARATOR_FIELD:
        return &JSAtomState::decimal;

      case UNUM_FRACTION_FIELD:
        return &JSAtomState::fraction;

      case UNUM_SIGN_FIELD: {
        MOZ_ASSERT(!IsNegativeZero(d),
                   "-0 should have been excluded by PartitionNumberPattern");

        // Manual trawling through the ICU call graph appears to indicate that
        // the basic formatting we request will never include a positive sign.
        // But this analysis may be mistaken, so don't absolutely trust it.
        return d < 0 ? &JSAtomState::minusSign : &JSAtomState::plusSign;
      }

      case UNUM_PERCENT_FIELD:
        return &JSAtomState::percentSign;

      case UNUM_CURRENCY_FIELD:
        return &JSAtomState::currency;

      case UNUM_PERMILL_FIELD:
        MOZ_ASSERT_UNREACHABLE("unexpected permill field found, even though "
                               "we don't use any user-defined patterns that "
                               "would require a permill field");
        break;

      case UNUM_EXPONENT_SYMBOL_FIELD:
      case UNUM_EXPONENT_SIGN_FIELD:
      case UNUM_EXPONENT_FIELD:
        MOZ_ASSERT_UNREACHABLE("exponent field unexpectedly found in "
                               "formatted number, even though UNUM_SCIENTIFIC "
                               "and scientific notation were never requested");
        break;

#ifndef U_HIDE_DEPRECATED_API
      case UNUM_FIELD_COUNT:
        MOZ_ASSERT_UNREACHABLE("format field sentinel value returned by "
                               "iterator!");
        break;
#endif
    }

    MOZ_ASSERT_UNREACHABLE("unenumerated, undocumented format field returned "
                           "by iterator");
    return nullptr;
}

static bool
intl_FormatNumberToParts(JSContext* cx, UNumberFormat* nf, double x, MutableHandleValue result)
{
    UErrorCode status = U_ZERO_ERROR;

    UFieldPositionIterator* fpositer = ufieldpositer_open(&status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }

    MOZ_ASSERT(fpositer);
    ScopedICUObject<UFieldPositionIterator, ufieldpositer_close> toClose(fpositer);

    RootedString overallResult(cx, PartitionNumberPattern(cx, nf, &x, fpositer));
    if (!overallResult)
        return false;

    RootedArrayObject partsArray(cx, NewDenseEmptyArray(cx));
    if (!partsArray)
        return false;

    // First, vacuum up fields in the overall formatted string.

    struct Field
    {
        uint32_t begin;
        uint32_t end;
        FieldType type;

        // Needed for vector-resizing scratch space.
        Field() = default;

        Field(uint32_t begin, uint32_t end, FieldType type)
          : begin(begin), end(end), type(type)
        {}
    };

    using FieldsVector = Vector<Field, 16>;
    FieldsVector fields(cx);

    int32_t fieldInt, beginIndexInt, endIndexInt;
    while ((fieldInt = ufieldpositer_next(fpositer, &beginIndexInt, &endIndexInt)) >= 0) {
        MOZ_ASSERT(beginIndexInt >= 0);
        MOZ_ASSERT(endIndexInt >= 0);
        MOZ_ASSERT(beginIndexInt < endIndexInt,
                   "erm, aren't fields always non-empty?");

        FieldType type = GetFieldTypeForNumberField(UNumberFormatFields(fieldInt), x);
        if (!fields.emplaceBack(uint32_t(beginIndexInt), uint32_t(endIndexInt), type))
            return false;
    }

    // Second, merge sort the fields vector.  Expand the vector to have scratch
    // space for performing the sort.
    size_t fieldsLen = fields.length();
    if (!fields.resizeUninitialized(fieldsLen * 2))
        return false;

    MOZ_ALWAYS_TRUE(MergeSort(fields.begin(), fieldsLen, fields.begin() + fieldsLen,
                              [](const Field& left, const Field& right,
                                 bool* lessOrEqual)
                              {
                                  // Sort first by begin index, then to place
                                  // enclosing fields before nested fields.
                                  *lessOrEqual = left.begin < right.begin ||
                                                 (left.begin == right.begin &&
                                                  left.end > right.end);
                                  return true;
                              }));

    // Deallocate the scratch space.
    if (!fields.resize(fieldsLen))
        return false;

    // Third, iterate over the sorted field list to generate a sequence of
    // parts (what ECMA-402 actually exposes).  A part is a maximal character
    // sequence entirely within no field or a single most-nested field.
    //
    // Diagrams may be helpful to illustrate how fields map to parts.  Consider
    // formatting -19,766,580,028,249.41, the US national surplus (negative
    // because it's actually a debt) on October 18, 2016.
    //
    //    var options =
    //      { style: "currency", currency: "USD", currencyDisplay: "name" };
    //    var usdFormatter = new Intl.NumberFormat("en-US", options);
    //    usdFormatter.format(-19766580028249.41);
    //
    // The formatted result is "-19,766,580,028,249.41 US dollars".  ICU
    // identifies these fields in the string:
    //
    //     UNUM_GROUPING_SEPARATOR_FIELD
    //                   |
    //   UNUM_SIGN_FIELD |  UNUM_DECIMAL_SEPARATOR_FIELD
    //    |   __________/|   |
    //    |  /   |   |   |   |
    //   "-19,766,580,028,249.41 US dollars"
    //     \________________/ |/ \_______/
    //             |          |      |
    //    UNUM_INTEGER_FIELD  |  UNUM_CURRENCY_FIELD
    //                        |
    //               UNUM_FRACTION_FIELD
    //
    // These fields map to parts as follows:
    //
    //         integer     decimal
    //       _____|________  |
    //      /  /| |\  |\  |\ |  literal
    //     /| / | | \ | \ | \|  |
    //   "-19,766,580,028,249.41 US dollars"
    //    |  \___|___|___/    |/ \________/
    //    |        |          |       |
    //    |      group        |   currency
    //    |                   |
    //   minusSign        fraction
    //
    // The sign is a part.  Each comma is a part, splitting the integer field
    // into parts for trillions/billions/&c. digits.  The decimal point is a
    // part.  Cents are a part.  The space between cents and currency is a part
    // (outside any field).  Last, the currency field is a part.
    //
    // Because parts fully partition the formatted string, we only track the
    // end of each part -- the beginning is implicitly the last part's end.
    struct Part
    {
        uint32_t end;
        FieldType type;
    };

    class PartGenerator
    {
        // The fields in order from start to end, then least to most nested.
        const FieldsVector& fields;

        // Index of the current field, in |fields|, being considered to
        // determine part boundaries.  |lastEnd <= fields[index].begin| is an
        // invariant.
        size_t index;

        // The end index of the last part produced, always less than or equal
        // to |limit|, strictly increasing.
        uint32_t lastEnd;

        // The length of the overall formatted string.
        const uint32_t limit;

        Vector<size_t, 4> enclosingFields;

        void popEnclosingFieldsEndingAt(uint32_t end) {
            MOZ_ASSERT_IF(enclosingFields.length() > 0,
                          fields[enclosingFields.back()].end >= end);

            while (enclosingFields.length() > 0 && fields[enclosingFields.back()].end == end)
                enclosingFields.popBack();
        }

        bool nextPartInternal(Part* part) {
            size_t len = fields.length();
            MOZ_ASSERT(index <= len);

            // If we're out of fields, all that remains are part(s) consisting
            // of trailing portions of enclosing fields, and maybe a final
            // literal part.
            if (index == len) {
                if (enclosingFields.length() > 0) {
                    const auto& enclosing = fields[enclosingFields.popCopy()];
                    part->end = enclosing.end;
                    part->type = enclosing.type;

                    // If additional enclosing fields end where this part ends,
                    // pop them as well.
                    popEnclosingFieldsEndingAt(part->end);
                } else {
                    part->end = limit;
                    part->type = &JSAtomState::literal;
                }

                return true;
            }

            // Otherwise we still have a field to process.
            const Field* current = &fields[index];
            MOZ_ASSERT(lastEnd <= current->begin);
            MOZ_ASSERT(current->begin < current->end);

            // But first, deal with inter-field space.
            if (lastEnd < current->begin) {
                if (enclosingFields.length() > 0) {
                    // Space between fields, within an enclosing field, is part
                    // of that enclosing field, until the start of the current
                    // field or the end of the enclosing field, whichever is
                    // earlier.
                    const auto& enclosing = fields[enclosingFields.back()];
                    part->end = std::min(enclosing.end, current->begin);
                    part->type = enclosing.type;
                    popEnclosingFieldsEndingAt(part->end);
                } else {
                    // If there's no enclosing field, the space is a literal.
                    part->end = current->begin;
                    part->type = &JSAtomState::literal;
                }

                return true;
            }

            // Otherwise, the part spans a prefix of the current field.  Find
            // the most-nested field containing that prefix.
            const Field* next;
            do {
                current = &fields[index];

                // If the current field is last, the part extends to its end.
                if (++index == len) {
                    part->end = current->end;
                    part->type = current->type;
                    return true;
                }

                next = &fields[index];
                MOZ_ASSERT(current->begin <= next->begin);
                MOZ_ASSERT(current->begin < next->end);

                // If the next field nests within the current field, push an
                // enclosing field.  (If there are no nested fields, don't
                // bother pushing a field that'd be immediately popped.)
                if (current->end > next->begin) {
                    if (!enclosingFields.append(index - 1))
                        return false;
                }

                // Do so until the next field begins after this one.
            } while (current->begin == next->begin);

            part->type = current->type;

            if (current->end <= next->begin) {
                // The next field begins after the current field ends.  Therefore
                // the current part ends at the end of the current field.
                part->end = current->end;
                popEnclosingFieldsEndingAt(part->end);
            } else {
                // The current field encloses the next one.  The current part
                // ends where the next field/part will start.
                part->end = next->begin;
            }

            return true;
        }

      public:
        PartGenerator(JSContext* cx, const FieldsVector& vec, uint32_t limit)
          : fields(vec), index(0), lastEnd(0), limit(limit), enclosingFields(cx)
        {}

        bool nextPart(bool* hasPart, Part* part) {
            // There are no parts left if we've partitioned the entire string.
            if (lastEnd == limit) {
                MOZ_ASSERT(enclosingFields.length() == 0);
                *hasPart = false;
                return true;
            }

            if (!nextPartInternal(part))
                return false;

            *hasPart = true;
            lastEnd = part->end;
            return true;
        }
    };

    // Finally, generate the result array.
    size_t lastEndIndex = 0;
    uint32_t partIndex = 0;
    RootedObject singlePart(cx);
    RootedValue propVal(cx);

    PartGenerator gen(cx, fields, overallResult->length());
    do {
        bool hasPart;
        Part part;
        if (!gen.nextPart(&hasPart, &part))
            return false;

        if (!hasPart)
            break;

        FieldType type = part.type;
        size_t endIndex = part.end;

        MOZ_ASSERT(lastEndIndex < endIndex);

        singlePart = NewBuiltinClassInstance<PlainObject>(cx);
        if (!singlePart)
            return false;

        propVal.setString(cx->names().*type);
        if (!DefineDataProperty(cx, singlePart, cx->names().type, propVal))
            return false;

        JSLinearString* partSubstr =
            NewDependentString(cx, overallResult, lastEndIndex, endIndex - lastEndIndex);
        if (!partSubstr)
            return false;

        propVal.setString(partSubstr);
        if (!DefineDataProperty(cx, singlePart, cx->names().value, propVal))
            return false;

        propVal.setObject(*singlePart);
        if (!DefineDataElement(cx, partsArray, partIndex, propVal))
            return false;

        lastEndIndex = endIndex;
        partIndex++;
    } while (true);

    MOZ_ASSERT(lastEndIndex == overallResult->length(),
               "result array must partition the entire string");

    result.setObject(*partsArray);
    return true;
}

bool
js::intl_FormatNumber(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[1].isNumber());
    MOZ_ASSERT(args[2].isBoolean());

    Rooted<NumberFormatObject*> numberFormat(cx, &args[0].toObject().as<NumberFormatObject>());

    // Obtain a cached UNumberFormat object.
    void* priv =
        numberFormat->getReservedSlot(NumberFormatObject::UNUMBER_FORMAT_SLOT).toPrivate();
    UNumberFormat* nf = static_cast<UNumberFormat*>(priv);
    if (!nf) {
        nf = NewUNumberFormat(cx, numberFormat);
        if (!nf)
            return false;
        numberFormat->setReservedSlot(NumberFormatObject::UNUMBER_FORMAT_SLOT, PrivateValue(nf));
    }

    // Use the UNumberFormat to actually format the number.
    if (args[2].toBoolean())
        return intl_FormatNumberToParts(cx, nf, args[1].toNumber(), args.rval());

    return intl_FormatNumber(cx, nf, args[1].toNumber(), args.rval());
}
