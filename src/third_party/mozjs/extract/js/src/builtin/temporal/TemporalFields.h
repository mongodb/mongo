/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_TemporalFields_h
#define builtin_temporal_TemporalFields_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/EnumSet.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"

#include <array>
#include <initializer_list>
#include <iterator>

#include "jstypes.h"

#include "builtin/temporal/Calendar.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"

class JS_PUBLIC_API JSTracer;

namespace js {
class PlainObject;
}

namespace js::temporal {
enum class TemporalField {
  Year,
  Month,
  MonthCode,
  Day,
  Hour,
  Minute,
  Second,
  Millisecond,
  Microsecond,
  Nanosecond,
  Offset,
  Era,
  EraYear,
  TimeZone,
};

struct FieldDescriptors {
  mozilla::EnumSet<TemporalField> relevant;
  mozilla::EnumSet<TemporalField> required;

#ifdef DEBUG
  FieldDescriptors(mozilla::EnumSet<TemporalField> relevant,
                   mozilla::EnumSet<TemporalField> required)
      : relevant(relevant), required(required) {
    MOZ_ASSERT(relevant.contains(required),
               "required is a subset of the relevant fields");
  }
#endif
};

template <typename T, const auto& sorted>
class SortedEnumSet {
  mozilla::EnumSet<T> fields_;

 public:
  explicit SortedEnumSet(mozilla::EnumSet<T> fields) : fields_(fields) {}

  class Iterator {
    mozilla::EnumSet<T> fields_;
    size_t index_;

    void findNext() {
      while (index_ < sorted.size() && !fields_.contains(sorted[index_])) {
        index_++;
      }
    }

    void findPrevious() {
      while (index_ > 0 && !fields_.contains(sorted[index_])) {
        index_--;
      }
    }

   public:
    // Iterator traits.
    using difference_type = ptrdiff_t;
    using value_type = TemporalField;
    using pointer = TemporalField*;
    using reference = TemporalField&;
    using iterator_category = std::bidirectional_iterator_tag;

    Iterator(mozilla::EnumSet<T> fields, size_t index)
        : fields_(fields), index_(index) {
      findNext();
    }

    bool operator==(const Iterator& other) const {
      MOZ_ASSERT(fields_ == other.fields_);
      return index_ == other.index_;
    }

    bool operator!=(const Iterator& other) const { return !(*this == other); }

    auto operator*() const {
      MOZ_ASSERT(index_ < sorted.size());
      MOZ_ASSERT(fields_.contains(sorted[index_]));
      return sorted[index_];
    }

    auto& operator++() {
      MOZ_ASSERT(index_ < sorted.size());
      index_++;
      findNext();
      return *this;
    }

    auto operator++(int) {
      auto result = *this;
      ++(*this);
      return result;
    }

    auto& operator--() {
      MOZ_ASSERT(index_ > 0);
      index_--;
      findPrevious();
      return *this;
    }

    auto operator--(int) {
      auto result = *this;
      --(*this);
      return result;
    }
  };

  Iterator begin() const { return Iterator{fields_, 0}; };

  Iterator end() const { return Iterator{fields_, sorted.size()}; }
};

namespace detail {
static constexpr auto sortedTemporalFields = std::array{
    TemporalField::Day,         TemporalField::Era,
    TemporalField::EraYear,     TemporalField::Hour,
    TemporalField::Microsecond, TemporalField::Millisecond,
    TemporalField::Minute,      TemporalField::Month,
    TemporalField::MonthCode,   TemporalField::Nanosecond,
    TemporalField::Offset,      TemporalField::Second,
    TemporalField::TimeZone,    TemporalField::Year,
};
}

// TODO: Consider reordering TemporalField so we don't need this. Probably best
// to decide after <https://github.com/tc39/proposal-temporal/issues/2826> has
// landed.
using SortedTemporalFields =
    SortedEnumSet<TemporalField, detail::sortedTemporalFields>;

// Default values are specified in Table 15 [1]. `undefined` is replaced with
// an appropriate value based on the type, for example `double` fields use
// NaN whereas pointer fields use nullptr.
//
// [1] <https://tc39.es/proposal-temporal/#table-temporal-field-requirements>
struct MOZ_STACK_CLASS TemporalFields final {
  double year = mozilla::UnspecifiedNaN<double>();
  double month = mozilla::UnspecifiedNaN<double>();
  JSString* monthCode = nullptr;
  double day = mozilla::UnspecifiedNaN<double>();
  double hour = 0;
  double minute = 0;
  double second = 0;
  double millisecond = 0;
  double microsecond = 0;
  double nanosecond = 0;
  JSString* offset = nullptr;
  JSString* era = nullptr;
  double eraYear = mozilla::UnspecifiedNaN<double>();
  JS::Value timeZone = JS::UndefinedValue();

  TemporalFields() = default;

  void trace(JSTracer* trc);
};
}  // namespace js::temporal

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::TemporalFields, Wrapper> {
  const temporal::TemporalFields& fields() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  double year() const { return fields().year; }
  double month() const { return fields().month; }
  double day() const { return fields().day; }
  double hour() const { return fields().hour; }
  double minute() const { return fields().minute; }
  double second() const { return fields().second; }
  double millisecond() const { return fields().millisecond; }
  double microsecond() const { return fields().microsecond; }
  double nanosecond() const { return fields().nanosecond; }
  double eraYear() const { return fields().eraYear; }

  JS::Handle<JSString*> monthCode() const {
    return JS::Handle<JSString*>::fromMarkedLocation(&fields().monthCode);
  }
  JS::Handle<JSString*> offset() const {
    return JS::Handle<JSString*>::fromMarkedLocation(&fields().offset);
  }
  JS::Handle<JSString*> era() const {
    return JS::Handle<JSString*>::fromMarkedLocation(&fields().era);
  }
  JS::Handle<JS::Value> timeZone() const {
    return JS::Handle<JS::Value>::fromMarkedLocation(&fields().timeZone);
  }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<temporal::TemporalFields, Wrapper>
    : public WrappedPtrOperations<temporal::TemporalFields, Wrapper> {
  temporal::TemporalFields& fields() {
    return static_cast<Wrapper*>(this)->get();
  }

 public:
  double& year() { return fields().year; }
  double& month() { return fields().month; }
  double& day() { return fields().day; }
  double& hour() { return fields().hour; }
  double& minute() { return fields().minute; }
  double& second() { return fields().second; }
  double& millisecond() { return fields().millisecond; }
  double& microsecond() { return fields().microsecond; }
  double& nanosecond() { return fields().nanosecond; }
  double& eraYear() { return fields().eraYear; }

  JS::MutableHandle<JSString*> monthCode() {
    return JS::MutableHandle<JSString*>::fromMarkedLocation(
        &fields().monthCode);
  }
  JS::MutableHandle<JSString*> offset() {
    return JS::MutableHandle<JSString*>::fromMarkedLocation(&fields().offset);
  }
  JS::MutableHandle<JSString*> era() {
    return JS::MutableHandle<JSString*>::fromMarkedLocation(&fields().era);
  }
  JS::MutableHandle<JS::Value> timeZone() {
    return JS::MutableHandle<JS::Value>::fromMarkedLocation(&fields().timeZone);
  }
};

}  // namespace js

namespace js::temporal {

PropertyName* ToPropertyName(JSContext* cx, TemporalField field);

mozilla::Maybe<TemporalField> ToTemporalField(JSContext* cx,
                                              PropertyKey property);

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * extraFieldDescriptors [ , duplicateBehaviour ] ] )
 */
bool PrepareTemporalFields(JSContext* cx, JS::Handle<JSObject*> fields,
                           mozilla::EnumSet<TemporalField> fieldNames,
                           mozilla::EnumSet<TemporalField> requiredFields,
                           JS::MutableHandle<TemporalFields> result);

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * extraFieldDescriptors [ , duplicateBehaviour ] ] )
 */
inline bool PrepareTemporalFields(
    JSContext* cx, JS::Handle<JSObject*> fields,
    mozilla::EnumSet<TemporalField> fieldNames,
    mozilla::EnumSet<TemporalField> requiredFields,
    const FieldDescriptors& extraFieldDescriptors,
    JS::MutableHandle<TemporalFields> result) {
  return PrepareTemporalFields(
      cx, fields, fieldNames + extraFieldDescriptors.relevant,
      requiredFields + extraFieldDescriptors.required, result);
}

using TemporalFieldNames = JS::StackGCVector<JS::PropertyKey>;

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * extraFieldDescriptors [ , duplicateBehaviour ] ] )
 */
PlainObject* PrepareTemporalFields(JSContext* cx, JS::Handle<JSObject*> fields,
                                   JS::Handle<TemporalFieldNames> fieldNames);

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * extraFieldDescriptors [ , duplicateBehaviour ] ] )
 */
PlainObject* PrepareTemporalFields(
    JSContext* cx, JS::Handle<JSObject*> fields,
    JS::Handle<TemporalFieldNames> fieldNames,
    mozilla::EnumSet<TemporalField> requiredFields);

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * extraFieldDescriptors [ , duplicateBehaviour ] ] )
 */
PlainObject* PreparePartialTemporalFields(
    JSContext* cx, JS::Handle<JSObject*> fields,
    JS::Handle<TemporalFieldNames> fieldNames);

/**
 * PrepareCalendarFieldsAndFieldNames ( calendarRec, fields, calendarFieldNames
 * [ , nonCalendarFieldNames [ , requiredFieldNames ] ] )
 */
bool PrepareCalendarFieldsAndFieldNames(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<JSObject*> fields,
    mozilla::EnumSet<CalendarField> calendarFieldNames,
    JS::MutableHandle<PlainObject*> resultFields,
    JS::MutableHandle<TemporalFieldNames> resultFieldNames);

/**
 * PrepareCalendarFields ( calendarRec, fields, calendarFieldNames,
 * nonCalendarFieldNames, requiredFieldNames )
 */
PlainObject* PrepareCalendarFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<JSObject*> fields,
    mozilla::EnumSet<CalendarField> calendarFieldNames,
    mozilla::EnumSet<TemporalField> nonCalendarFieldNames = {},
    mozilla::EnumSet<TemporalField> requiredFieldNames = {});

[[nodiscard]] bool ConcatTemporalFieldNames(
    const TemporalFieldNames& receiverFieldNames,
    const TemporalFieldNames& inputFieldNames,
    TemporalFieldNames& concatenatedFieldNames);

[[nodiscard]] bool AppendSorted(
    JSContext* cx, TemporalFieldNames& fieldNames,
    mozilla::EnumSet<TemporalField> additionalNames);

[[nodiscard]] bool SortTemporalFieldNames(JSContext* cx,
                                          TemporalFieldNames& fieldNames);
} /* namespace js::temporal */

#endif /* builtin_temporal_TemporalFields_h */
