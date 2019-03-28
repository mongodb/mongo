// Formatting library for C++ - chrono support
//
// Copyright (c) 2012 - present, Victor Zverovich
// All rights reserved.
//
// For the license information refer to format.h.

#ifndef FMT_CHRONO_H_
#define FMT_CHRONO_H_

#include "format.h"
#include "locale.h"

#include <chrono>
#include <ctime>
#include <locale>
#include <sstream>

FMT_BEGIN_NAMESPACE

// Prevents expansion of a preceding token as a function-style macro.
// Usage: f FMT_NOMACRO()
#define FMT_NOMACRO

namespace internal {
inline null<> localtime_r FMT_NOMACRO(...) { return null<>(); }
inline null<> localtime_s(...) { return null<>(); }
inline null<> gmtime_r(...) { return null<>(); }
inline null<> gmtime_s(...) { return null<>(); }
}  // namespace internal

// Thread-safe replacement for std::localtime
inline std::tm localtime(std::time_t time) {
  struct dispatcher {
    std::time_t time_;
    std::tm tm_;

    dispatcher(std::time_t t) : time_(t) {}

    bool run() {
      using namespace fmt::internal;
      return handle(localtime_r(&time_, &tm_));
    }

    bool handle(std::tm* tm) { return tm != FMT_NULL; }

    bool handle(internal::null<>) {
      using namespace fmt::internal;
      return fallback(localtime_s(&tm_, &time_));
    }

    bool fallback(int res) { return res == 0; }

#if !FMT_MSC_VER
    bool fallback(internal::null<>) {
      using namespace fmt::internal;
      std::tm* tm = std::localtime(&time_);
      if (tm) tm_ = *tm;
      return tm != FMT_NULL;
    }
#endif
  };
  dispatcher lt(time);
  // Too big time values may be unsupported.
  if (!lt.run()) FMT_THROW(format_error("time_t value out of range"));
  return lt.tm_;
}

// Thread-safe replacement for std::gmtime
inline std::tm gmtime(std::time_t time) {
  struct dispatcher {
    std::time_t time_;
    std::tm tm_;

    dispatcher(std::time_t t) : time_(t) {}

    bool run() {
      using namespace fmt::internal;
      return handle(gmtime_r(&time_, &tm_));
    }

    bool handle(std::tm* tm) { return tm != FMT_NULL; }

    bool handle(internal::null<>) {
      using namespace fmt::internal;
      return fallback(gmtime_s(&tm_, &time_));
    }

    bool fallback(int res) { return res == 0; }

#if !FMT_MSC_VER
    bool fallback(internal::null<>) {
      std::tm* tm = std::gmtime(&time_);
      if (tm) tm_ = *tm;
      return tm != FMT_NULL;
    }
#endif
  };
  dispatcher gt(time);
  // Too big time values may be unsupported.
  if (!gt.run()) FMT_THROW(format_error("time_t value out of range"));
  return gt.tm_;
}

namespace internal {
inline std::size_t strftime(char* str, std::size_t count, const char* format,
                            const std::tm* time) {
  return std::strftime(str, count, format, time);
}

inline std::size_t strftime(wchar_t* str, std::size_t count,
                            const wchar_t* format, const std::tm* time) {
  return std::wcsftime(str, count, format, time);
}
}  // namespace internal

template <typename Char> struct formatter<std::tm, Char> {
  template <typename ParseContext>
  auto parse(ParseContext& ctx) -> decltype(ctx.begin()) {
    auto it = ctx.begin();
    if (it != ctx.end() && *it == ':') ++it;
    auto end = it;
    while (end != ctx.end() && *end != '}') ++end;
    tm_format.reserve(internal::to_unsigned(end - it + 1));
    tm_format.append(it, end);
    tm_format.push_back('\0');
    return end;
  }

  template <typename FormatContext>
  auto format(const std::tm& tm, FormatContext& ctx) -> decltype(ctx.out()) {
    basic_memory_buffer<Char> buf;
    std::size_t start = buf.size();
    for (;;) {
      std::size_t size = buf.capacity() - start;
      std::size_t count =
          internal::strftime(&buf[start], size, &tm_format[0], &tm);
      if (count != 0) {
        buf.resize(start + count);
        break;
      }
      if (size >= tm_format.size() * 256) {
        // If the buffer is 256 times larger than the format string, assume
        // that `strftime` gives an empty result. There doesn't seem to be a
        // better way to distinguish the two cases:
        // https://github.com/fmtlib/fmt/issues/367
        break;
      }
      const std::size_t MIN_GROWTH = 10;
      buf.reserve(buf.capacity() + (size > MIN_GROWTH ? size : MIN_GROWTH));
    }
    return std::copy(buf.begin(), buf.end(), ctx.out());
  }

  basic_memory_buffer<Char> tm_format;
};

namespace internal {
template <typename Period> FMT_CONSTEXPR const char* get_units() {
  return FMT_NULL;
}
template <> FMT_CONSTEXPR const char* get_units<std::atto>() { return "as"; }
template <> FMT_CONSTEXPR const char* get_units<std::femto>() { return "fs"; }
template <> FMT_CONSTEXPR const char* get_units<std::pico>() { return "ps"; }
template <> FMT_CONSTEXPR const char* get_units<std::nano>() { return "ns"; }
template <> FMT_CONSTEXPR const char* get_units<std::micro>() { return "µs"; }
template <> FMT_CONSTEXPR const char* get_units<std::milli>() { return "ms"; }
template <> FMT_CONSTEXPR const char* get_units<std::centi>() { return "cs"; }
template <> FMT_CONSTEXPR const char* get_units<std::deci>() { return "ds"; }
template <> FMT_CONSTEXPR const char* get_units<std::ratio<1>>() { return "s"; }
template <> FMT_CONSTEXPR const char* get_units<std::deca>() { return "das"; }
template <> FMT_CONSTEXPR const char* get_units<std::hecto>() { return "hs"; }
template <> FMT_CONSTEXPR const char* get_units<std::kilo>() { return "ks"; }
template <> FMT_CONSTEXPR const char* get_units<std::mega>() { return "Ms"; }
template <> FMT_CONSTEXPR const char* get_units<std::giga>() { return "Gs"; }
template <> FMT_CONSTEXPR const char* get_units<std::tera>() { return "Ts"; }
template <> FMT_CONSTEXPR const char* get_units<std::peta>() { return "Ps"; }
template <> FMT_CONSTEXPR const char* get_units<std::exa>() { return "Es"; }
template <> FMT_CONSTEXPR const char* get_units<std::ratio<60>>() {
  return "m";
}
template <> FMT_CONSTEXPR const char* get_units<std::ratio<3600>>() {
  return "h";
}

enum class numeric_system {
  standard,
  // Alternative numeric system, e.g. 十二 instead of 12 in ja_JP locale.
  alternative
};

// Parses a put_time-like format string and invokes handler actions.
template <typename Char, typename Handler>
FMT_CONSTEXPR const Char* parse_chrono_format(const Char* begin,
                                              const Char* end,
                                              Handler&& handler) {
  auto ptr = begin;
  while (ptr != end) {
    auto c = *ptr;
    if (c == '}') break;
    if (c != '%') {
      ++ptr;
      continue;
    }
    if (begin != ptr) handler.on_text(begin, ptr);
    ++ptr;  // consume '%'
    if (ptr == end) FMT_THROW(format_error("invalid format"));
    c = *ptr++;
    switch (c) {
    case '%':
      handler.on_text(ptr - 1, ptr);
      break;
    case 'n': {
      const char newline[] = "\n";
      handler.on_text(newline, newline + 1);
      break;
    }
    case 't': {
      const char tab[] = "\t";
      handler.on_text(tab, tab + 1);
      break;
    }
    // Day of the week:
    case 'a':
      handler.on_abbr_weekday();
      break;
    case 'A':
      handler.on_full_weekday();
      break;
    case 'w':
      handler.on_dec0_weekday(numeric_system::standard);
      break;
    case 'u':
      handler.on_dec1_weekday(numeric_system::standard);
      break;
    // Month:
    case 'b':
      handler.on_abbr_month();
      break;
    case 'B':
      handler.on_full_month();
      break;
    // Hour, minute, second:
    case 'H':
      handler.on_24_hour(numeric_system::standard);
      break;
    case 'I':
      handler.on_12_hour(numeric_system::standard);
      break;
    case 'M':
      handler.on_minute(numeric_system::standard);
      break;
    case 'S':
      handler.on_second(numeric_system::standard);
      break;
    // Other:
    case 'c':
      handler.on_datetime(numeric_system::standard);
      break;
    case 'x':
      handler.on_loc_date(numeric_system::standard);
      break;
    case 'X':
      handler.on_loc_time(numeric_system::standard);
      break;
    case 'D':
      handler.on_us_date();
      break;
    case 'F':
      handler.on_iso_date();
      break;
    case 'r':
      handler.on_12_hour_time();
      break;
    case 'R':
      handler.on_24_hour_time();
      break;
    case 'T':
      handler.on_iso_time();
      break;
    case 'p':
      handler.on_am_pm();
      break;
    case 'Q':
      handler.on_duration_value();
      break;
    case 'q':
      handler.on_duration_unit();
      break;
    case 'z':
      handler.on_utc_offset();
      break;
    case 'Z':
      handler.on_tz_name();
      break;
    // Alternative representation:
    case 'E': {
      if (ptr == end) FMT_THROW(format_error("invalid format"));
      c = *ptr++;
      switch (c) {
      case 'c':
        handler.on_datetime(numeric_system::alternative);
        break;
      case 'x':
        handler.on_loc_date(numeric_system::alternative);
        break;
      case 'X':
        handler.on_loc_time(numeric_system::alternative);
        break;
      default:
        FMT_THROW(format_error("invalid format"));
      }
      break;
    }
    case 'O':
      if (ptr == end) FMT_THROW(format_error("invalid format"));
      c = *ptr++;
      switch (c) {
      case 'w':
        handler.on_dec0_weekday(numeric_system::alternative);
        break;
      case 'u':
        handler.on_dec1_weekday(numeric_system::alternative);
        break;
      case 'H':
        handler.on_24_hour(numeric_system::alternative);
        break;
      case 'I':
        handler.on_12_hour(numeric_system::alternative);
        break;
      case 'M':
        handler.on_minute(numeric_system::alternative);
        break;
      case 'S':
        handler.on_second(numeric_system::alternative);
        break;
      default:
        FMT_THROW(format_error("invalid format"));
      }
      break;
    default:
      FMT_THROW(format_error("invalid format"));
    }
    begin = ptr;
  }
  if (begin != ptr) handler.on_text(begin, ptr);
  return ptr;
}

struct chrono_format_checker {
  void report_no_date() { FMT_THROW(format_error("no date")); }

  template <typename Char> void on_text(const Char*, const Char*) {}
  void on_abbr_weekday() { report_no_date(); }
  void on_full_weekday() { report_no_date(); }
  void on_dec0_weekday(numeric_system) { report_no_date(); }
  void on_dec1_weekday(numeric_system) { report_no_date(); }
  void on_abbr_month() { report_no_date(); }
  void on_full_month() { report_no_date(); }
  void on_24_hour(numeric_system) {}
  void on_12_hour(numeric_system) {}
  void on_minute(numeric_system) {}
  void on_second(numeric_system) {}
  void on_datetime(numeric_system) { report_no_date(); }
  void on_loc_date(numeric_system) { report_no_date(); }
  void on_loc_time(numeric_system) { report_no_date(); }
  void on_us_date() { report_no_date(); }
  void on_iso_date() { report_no_date(); }
  void on_12_hour_time() {}
  void on_24_hour_time() {}
  void on_iso_time() {}
  void on_am_pm() {}
  void on_duration_value() {}
  void on_duration_unit() {}
  void on_utc_offset() { report_no_date(); }
  void on_tz_name() { report_no_date(); }
};

template <typename Int> inline int to_int(Int value) {
  FMT_ASSERT(value >= (std::numeric_limits<int>::min)() &&
                 value <= (std::numeric_limits<int>::max)(),
             "invalid value");
  return static_cast<int>(value);
}

template <typename Rep, typename OutputIt>
OutputIt static format_chrono_duration_value(OutputIt out, Rep val,
                                             int precision) {
  if (precision < 0) {
    return format_to(out, std::is_floating_point<Rep>::value ? "{:g}" : "{}",
                     val);
  }
  return format_to(out, "{:.{}f}", val, precision);
}

template <typename Period, typename OutputIt>
static OutputIt format_chrono_duration_unit(OutputIt out) {
  if (const char* unit = get_units<Period>())
    return format_to(out, "{}", unit);
  else if (Period::den == 1)
    return format_to(out, "[{}]s", Period::num);
  else
    return format_to(out, "[{}/{}]s", Period::num, Period::den);
}

template <typename FormatContext, typename OutputIt, typename Rep,
          typename Period>
struct chrono_formatter {
  FormatContext& context;
  OutputIt out;
  int precision;
  Rep val;
  typedef std::chrono::duration<Rep, std::milli> milliseconds;
  std::chrono::seconds s;
  milliseconds ms;

  typedef typename FormatContext::char_type char_type;

  explicit chrono_formatter(FormatContext& ctx, OutputIt o,
                            std::chrono::duration<Rep, Period> d)
      : context(ctx),
        out(o),
        val(d.count()),
        s(std::chrono::duration_cast<std::chrono::seconds>(d)),
        ms(std::chrono::duration_cast<milliseconds>(d - s)) {}

  int hour() const { return to_int((s.count() / 3600) % 24); }

  int hour12() const {
    auto hour = to_int((s.count() / 3600) % 12);
    return hour > 0 ? hour : 12;
  }

  int minute() const { return to_int((s.count() / 60) % 60); }
  int second() const { return to_int(s.count() % 60); }

  std::tm time() const {
    auto time = std::tm();
    time.tm_hour = hour();
    time.tm_min = minute();
    time.tm_sec = second();
    return time;
  }

  void write(int value, int width) {
    typedef typename int_traits<int>::main_type main_type;
    main_type n = to_unsigned(value);
    int num_digits = internal::count_digits(n);
    if (width > num_digits) out = std::fill_n(out, width - num_digits, '0');
    out = format_decimal<char_type>(out, n, num_digits);
  }

  void format_localized(const tm& time, const char* format) {
    auto locale = context.locale().template get<std::locale>();
    auto& facet = std::use_facet<std::time_put<char_type>>(locale);
    std::basic_ostringstream<char_type> os;
    os.imbue(locale);
    facet.put(os, os, ' ', &time, format, format + std::strlen(format));
    auto str = os.str();
    std::copy(str.begin(), str.end(), out);
  }

  void on_text(const char_type* begin, const char_type* end) {
    std::copy(begin, end, out);
  }

  // These are not implemented because durations don't have date information.
  void on_abbr_weekday() {}
  void on_full_weekday() {}
  void on_dec0_weekday(numeric_system) {}
  void on_dec1_weekday(numeric_system) {}
  void on_abbr_month() {}
  void on_full_month() {}
  void on_datetime(numeric_system) {}
  void on_loc_date(numeric_system) {}
  void on_loc_time(numeric_system) {}
  void on_us_date() {}
  void on_iso_date() {}
  void on_utc_offset() {}
  void on_tz_name() {}

  void on_24_hour(numeric_system ns) {
    if (ns == numeric_system::standard) return write(hour(), 2);
    auto time = tm();
    time.tm_hour = hour();
    format_localized(time, "%OH");
  }

  void on_12_hour(numeric_system ns) {
    if (ns == numeric_system::standard) return write(hour12(), 2);
    auto time = tm();
    time.tm_hour = hour();
    format_localized(time, "%OI");
  }

  void on_minute(numeric_system ns) {
    if (ns == numeric_system::standard) return write(minute(), 2);
    auto time = tm();
    time.tm_min = minute();
    format_localized(time, "%OM");
  }

  void on_second(numeric_system ns) {
    if (ns == numeric_system::standard) {
      write(second(), 2);
      if (ms != std::chrono::milliseconds(0)) {
        *out++ = '.';
        write(to_int(ms.count()), 3);
      }
      return;
    }
    auto time = tm();
    time.tm_sec = second();
    format_localized(time, "%OS");
  }

  void on_12_hour_time() { format_localized(time(), "%r"); }

  void on_24_hour_time() {
    write(hour(), 2);
    *out++ = ':';
    write(minute(), 2);
  }

  void on_iso_time() {
    on_24_hour_time();
    *out++ = ':';
    write(second(), 2);
  }

  void on_am_pm() { format_localized(time(), "%p"); }

  void on_duration_value() {
    out = format_chrono_duration_value(out, val, precision);
  }

  void on_duration_unit() { out = format_chrono_duration_unit<Period>(out); }
};
}  // namespace internal

template <typename Rep, typename Period, typename Char>
struct formatter<std::chrono::duration<Rep, Period>, Char> {
 private:
  align_spec spec;
  int precision;
  typedef internal::arg_ref<Char> arg_ref_type;
  arg_ref_type width_ref;
  arg_ref_type precision_ref;
  mutable basic_string_view<Char> format_str;
  typedef std::chrono::duration<Rep, Period> duration;

  struct spec_handler {
    formatter& f;
    basic_parse_context<Char>& context;
    basic_string_view<Char> format_str;

    template <typename Id> FMT_CONSTEXPR arg_ref_type make_arg_ref(Id arg_id) {
      context.check_arg_id(arg_id);
      return arg_ref_type(arg_id);
    }

    FMT_CONSTEXPR arg_ref_type make_arg_ref(basic_string_view<Char> arg_id) {
      context.check_arg_id(arg_id);
      const auto str_val = internal::string_view_metadata(format_str, arg_id);
      return arg_ref_type(str_val);
    }

    FMT_CONSTEXPR arg_ref_type make_arg_ref(internal::auto_id) {
      return arg_ref_type(context.next_arg_id());
    }

    void on_error(const char* msg) { FMT_THROW(format_error(msg)); }
    void on_fill(Char fill) { f.spec.fill_ = fill; }
    void on_align(alignment align) { f.spec.align_ = align; }
    void on_width(unsigned width) { f.spec.width_ = width; }
    void on_precision(unsigned precision) { f.precision = precision; }
    void end_precision() {}

    template <typename Id> void on_dynamic_width(Id arg_id) {
      f.width_ref = make_arg_ref(arg_id);
    }

    template <typename Id> void on_dynamic_precision(Id arg_id) {
      f.precision_ref = make_arg_ref(arg_id);
    }
  };

 public:
  formatter() : spec(), precision(-1) {}

  FMT_CONSTEXPR auto parse(basic_parse_context<Char>& ctx)
      -> decltype(ctx.begin()) {
    auto begin = ctx.begin(), end = ctx.end();
    if (begin == end) return begin;
    spec_handler handler{*this, ctx, format_str};
    begin = internal::parse_align(begin, end, handler);
    if (begin == end) return begin;
    begin = internal::parse_width(begin, end, handler);
    if (begin == end) return begin;
    if (*begin == '.') {
      if (std::is_floating_point<Rep>::value)
        begin = internal::parse_precision(begin, end, handler);
      else
        handler.on_error("precision not allowed for this argument type");
    }
    end = parse_chrono_format(begin, end, internal::chrono_format_checker());
    format_str =
        basic_string_view<Char>(&*begin, internal::to_unsigned(end - begin));
    return end;
  }

  template <typename FormatContext>
  auto format(const duration& d, FormatContext& ctx) -> decltype(ctx.out()) {
    auto begin = format_str.begin(), end = format_str.end();
    // As a possible future optimization, we could avoid extra copying if width
    // is not specified.
    basic_memory_buffer<Char> buf;
    auto out = std::back_inserter(buf);
    typedef output_range<decltype(ctx.out()), Char> range;
    basic_writer<range> w(range(ctx.out()));
    internal::handle_dynamic_spec<internal::width_checker>(
        spec.width_, width_ref, ctx, format_str.begin());
    internal::handle_dynamic_spec<internal::precision_checker>(
        precision, precision_ref, ctx, format_str.begin());
    if (begin == end || *begin == '}') {
      out = internal::format_chrono_duration_value(out, d.count(), precision);
      internal::format_chrono_duration_unit<Period>(out);
    } else {
      internal::chrono_formatter<FormatContext, decltype(out), Rep, Period> f(
          ctx, out, d);
      f.precision = precision;
      parse_chrono_format(begin, end, f);
    }
    w.write(buf.data(), buf.size(), spec);
    return w.out();
  }
};

FMT_END_NAMESPACE

#endif  // FMT_CHRONO_H_
