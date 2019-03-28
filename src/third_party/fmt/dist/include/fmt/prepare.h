// Formatting library for C++ - format string compilation
//
// Copyright (c) 2012 - present, Victor Zverovich and fmt contributors
// All rights reserved.
//
// For the license information refer to format.h.

#ifndef FMT_PREPARE_H_
#define FMT_PREPARE_H_

#ifndef FMT_HAS_CONSTRUCTIBLE_TRAITS
#  define FMT_HAS_CONSTRUCTIBLE_TRAITS \
    (FMT_GCC_VERSION >= 407 || FMT_CLANG_VERSION || FMT_MSC_VER)
#endif

#include "format.h"

#include <vector>

FMT_BEGIN_NAMESPACE

template <typename Char> struct format_part {
 public:
  struct named_argument_id {
    FMT_CONSTEXPR named_argument_id(internal::string_view_metadata id)
        : id(id) {}
    internal::string_view_metadata id;
  };

  struct argument_id {
    FMT_CONSTEXPR argument_id() : argument_id(0u) {}

    FMT_CONSTEXPR argument_id(unsigned id)
        : which(which_arg_id::index), val(id) {}

    FMT_CONSTEXPR argument_id(internal::string_view_metadata id)
        : which(which_arg_id::named_index), val(id) {}

    enum class which_arg_id { index, named_index };

    which_arg_id which;

    FMT_UNRESTRICTED_UNION value {
      FMT_CONSTEXPR value() : index(0u) {}
      FMT_CONSTEXPR value(unsigned id) : index(id) {}
      FMT_CONSTEXPR value(internal::string_view_metadata id)
          : named_index(id) {}

      unsigned index;
      internal::string_view_metadata named_index;
    }
    val;
  };

  struct specification {
    FMT_CONSTEXPR specification() : arg_id(0u) {}
    FMT_CONSTEXPR specification(unsigned id) : arg_id(id) {}

    FMT_CONSTEXPR specification(internal::string_view_metadata id)
        : arg_id(id) {}

    argument_id arg_id;
    internal::dynamic_format_specs<Char> parsed_specs;
  };

  FMT_CONSTEXPR format_part()
      : which(which_value::argument_id), end_of_argument_id(0u), val(0u) {}

  FMT_CONSTEXPR format_part(internal::string_view_metadata text)
      : which(which_value::text), end_of_argument_id(0u), val(text) {}

  FMT_CONSTEXPR format_part(unsigned id)
      : which(which_value::argument_id), end_of_argument_id(0u), val(id) {}

  FMT_CONSTEXPR format_part(named_argument_id arg_id)
      : which(which_value::named_argument_id),
        end_of_argument_id(0u),
        val(arg_id) {}

  FMT_CONSTEXPR format_part(specification spec)
      : which(which_value::specification), end_of_argument_id(0u), val(spec) {}

  enum class which_value {
    argument_id,
    named_argument_id,
    text,
    specification
  };

  which_value which;
  std::size_t end_of_argument_id;
  FMT_UNRESTRICTED_UNION value {
    FMT_CONSTEXPR value() : arg_id(0u) {}
    FMT_CONSTEXPR value(unsigned id) : arg_id(id) {}
    FMT_CONSTEXPR value(named_argument_id named_id)
        : named_arg_id(named_id.id) {}
    FMT_CONSTEXPR value(internal::string_view_metadata t) : text(t) {}
    FMT_CONSTEXPR value(specification s) : spec(s) {}
    unsigned arg_id;
    internal::string_view_metadata named_arg_id;
    internal::string_view_metadata text;
    specification spec;
  }
  val;
};

namespace internal {
template <typename Char, typename PartsContainer>
class format_preparation_handler : public internal::error_handler {
 private:
  typedef format_part<Char> part;

 public:
  typedef internal::null_terminating_iterator<Char> iterator;

  FMT_CONSTEXPR format_preparation_handler(basic_string_view<Char> format,
                                           PartsContainer& parts)
      : parts_(parts), format_(format), parse_context_(format) {}

  FMT_CONSTEXPR void on_text(const Char* begin, const Char* end) {
    if (begin == end) {
      return;
    }
    const auto offset = begin - format_.data();
    const auto size = end - begin;
    parts_.add(part(string_view_metadata(offset, size)));
  }

  FMT_CONSTEXPR void on_arg_id() {
    parts_.add(part(parse_context_.next_arg_id()));
  }

  FMT_CONSTEXPR void on_arg_id(unsigned id) {
    parse_context_.check_arg_id(id);
    parts_.add(part(id));
  }

  FMT_CONSTEXPR void on_arg_id(basic_string_view<Char> id) {
    const auto view = string_view_metadata(format_, id);
    const auto arg_id = typename part::named_argument_id(view);
    parts_.add(part(arg_id));
  }

  FMT_CONSTEXPR void on_replacement_field(const Char* ptr) {
    auto last_part = parts_.last();
    last_part.end_of_argument_id = ptr - format_.begin();
    parts_.substitute_last(last_part);
  }

  FMT_CONSTEXPR const Char* on_format_specs(const Char* begin,
                                            const Char* end) {
    const auto specs_offset = to_unsigned(begin - format_.begin());

    typedef basic_parse_context<Char> parse_context;
    internal::dynamic_format_specs<Char> parsed_specs;
    dynamic_specs_handler<parse_context> handler(parsed_specs, parse_context_);
    begin = parse_format_specs(begin, end, handler);

    if (*begin != '}') {
      on_error("missing '}' in format string");
    }

    const auto last_part = parts_.last();

    auto specs = last_part.which == part::which_value::argument_id
                     ? typename part::specification(last_part.val.arg_id)
                     : typename part::specification(last_part.val.named_arg_id);

    specs.parsed_specs = parsed_specs;

    auto new_part = part(specs);
    new_part.end_of_argument_id = specs_offset;

    parts_.substitute_last(new_part);

    return begin;
  }

 private:
  PartsContainer& parts_;
  basic_string_view<Char> format_;
  basic_parse_context<Char> parse_context_;
};

template <typename Format, typename PreparedPartsProvider, typename... Args>
class prepared_format {
 public:
  typedef FMT_CHAR(Format) char_type;
  typedef format_part<char_type> format_part_t;

  prepared_format(Format f)
      : format_(std::move(f)), parts_provider_(to_string_view(format_)) {}

  prepared_format() = delete;

  std::size_t formatted_size(const Args&... args) const {
    const auto it = this->format_to(counting_iterator<char_type>(), args...);
    return it.count();
  }

  template <typename OutputIt,
            FMT_ENABLE_IF(internal::is_output_iterator<OutputIt>::value)>
  inline format_to_n_result<OutputIt> format_to_n(OutputIt out, unsigned n,
                                                  const Args&... args) const {
    format_arg_store<typename format_to_n_context<OutputIt, char_type>::type,
                     Args...>
    as(args...);

    typedef truncating_iterator<OutputIt> trunc_it;
    typedef output_range<trunc_it, char_type> range;
    range r(trunc_it(out, n));
    auto it = this->vformat_to(
        r, typename format_to_n_args<OutputIt, char_type>::type(as));
    return {it.base(), it.count()};
  }

  std::basic_string<char_type> format(const Args&... args) const {
    basic_memory_buffer<char_type> buffer;
    typedef back_insert_range<internal::basic_buffer<char_type>> range;
    this->vformat_to(range(buffer), make_args_checked(format_, args...));
    return to_string(buffer);
  }

  template <typename Container, FMT_ENABLE_IF(is_contiguous<Container>::value)>
  inline std::back_insert_iterator<Container> format_to(
      std::back_insert_iterator<Container> out, const Args&... args) const {
    internal::container_buffer<Container> buffer(internal::get_container(out));
    typedef back_insert_range<internal::basic_buffer<char_type>> range;
    this->vformat_to(range(buffer), make_args_checked(format_, args...));
    return out;
  }

  template <typename OutputIt>
  inline OutputIt format_to(OutputIt out, const Args&... args) const {
    typedef typename format_context_t<OutputIt, char_type>::type context;
    typedef output_range<OutputIt, char_type> range;
    format_arg_store<context, Args...> as(args...);
    return this->vformat_to(range(out), basic_format_args<context>(as));
  }

  template <std::size_t SIZE = inline_buffer_size>
  inline typename buffer_context<char_type>::type::iterator format_to(
      basic_memory_buffer<char_type, SIZE>& buf, const Args&... args) const {
    typedef back_insert_range<internal::basic_buffer<char_type>> range;
    return this->vformat_to(range(buf), make_args_checked(format_, args...));
  }

 private:
  typedef typename buffer_context<char_type>::type context;

  template <typename Range>
  typename context::iterator vformat_to(Range out,
                                        basic_format_args<context> args) const {
    const auto format_view = internal::to_string_view(format_);
    basic_parse_context<char_type> parse_ctx(format_view);
    context ctx(out.begin(), args);

    const auto& parts = parts_provider_.parts();
    for (auto part_it = parts.begin(); part_it != parts.end(); ++part_it) {
      const auto& part = *part_it;
      const auto& value = part.val;

      switch (part.which) {
      case format_part_t::which_value::text: {
        const auto text = value.text.to_view(format_view);
        auto output = ctx.out();
        auto&& it = internal::reserve(output, text.size());
        it = std::copy_n(text.begin(), text.size(), it);
        ctx.advance_to(output);
      } break;

      case format_part_t::which_value::argument_id: {
        advance_parse_context_to_specification(parse_ctx, part);
        format_arg<Range>(parse_ctx, ctx, value.arg_id);
      } break;

      case format_part_t::which_value::named_argument_id: {
        advance_parse_context_to_specification(parse_ctx, part);
        const auto named_arg_id = value.named_arg_id.to_view(format_view);
        format_arg<Range>(parse_ctx, ctx, named_arg_id);
      } break;
      case format_part_t::which_value::specification: {
        const auto& arg_id_value = value.spec.arg_id.val;
        const auto arg =
            value.spec.arg_id.which ==
                    format_part_t::argument_id::which_arg_id::index
                ? ctx.arg(arg_id_value.index)
                : ctx.arg(arg_id_value.named_index.to_view(format_));

        auto specs = value.spec.parsed_specs;

        handle_dynamic_spec<internal::width_checker>(
            specs.width_, specs.width_ref, ctx, format_view.begin());
        handle_dynamic_spec<internal::precision_checker>(
            specs.precision, specs.precision_ref, ctx, format_view.begin());

        check_prepared_specs(specs, arg.type());
        advance_parse_context_to_specification(parse_ctx, part);
        ctx.advance_to(
            visit_format_arg(arg_formatter<Range>(ctx, FMT_NULL, &specs), arg));
      } break;
      }
    }

    return ctx.out();
  }

  void advance_parse_context_to_specification(
      basic_parse_context<char_type>& parse_ctx,
      const format_part_t& part) const {
    const auto view = to_string_view(format_);
    const auto specification_begin = view.data() + part.end_of_argument_id;
    parse_ctx.advance_to(specification_begin);
  }

  template <typename Range, typename Context, typename Id>
  void format_arg(basic_parse_context<char_type>& parse_ctx, Context& ctx,
                  Id arg_id) const {
    parse_ctx.check_arg_id(arg_id);
    const auto stopped_at =
        visit_format_arg(arg_formatter<Range>(ctx), ctx.arg(arg_id));
    ctx.advance_to(stopped_at);
  }

  template <typename Char>
  void check_prepared_specs(const basic_format_specs<Char>& specs,
                            internal::type arg_type) const {
    internal::error_handler h;
    numeric_specs_checker<internal::error_handler> checker(h, arg_type);
    if (specs.align_ == ALIGN_NUMERIC) {
      checker.require_numeric_argument();
    }

    if (specs.has(PLUS_FLAG | MINUS_FLAG | SIGN_FLAG)) {
      checker.check_sign();
    }

    if (specs.has(HASH_FLAG)) {
      checker.require_numeric_argument();
    }

    if (specs.has_precision()) {
      checker.check_precision();
    }
  }

 private:
  Format format_;
  PreparedPartsProvider parts_provider_;
};

template <typename Format> class compiletime_prepared_parts_type_provider {
 private:
  typedef FMT_CHAR(Format) char_type;

  class count_handler {
   private:
    typedef internal::null_terminating_iterator<char_type> iterator;

   public:
    FMT_CONSTEXPR count_handler() : counter_(0u) {}

    FMT_CONSTEXPR void on_text(const char_type* begin, const char_type* end) {
      if (begin != end) {
        ++counter_;
      }
    }

    FMT_CONSTEXPR void on_arg_id() { ++counter_; }
    FMT_CONSTEXPR void on_arg_id(unsigned) { ++counter_; }
    FMT_CONSTEXPR void on_arg_id(basic_string_view<char_type>) { ++counter_; }

    FMT_CONSTEXPR void on_replacement_field(const char_type*) {}

    FMT_CONSTEXPR const char_type* on_format_specs(const char_type* begin,
                                                   const char_type* end) {
      return find_matching_brace(begin, end);
    }

    FMT_CONSTEXPR void on_error(const char*) {}

    FMT_CONSTEXPR unsigned result() const { return counter_; }

   private:
    FMT_CONSTEXPR const char_type* find_matching_brace(const char_type* begin,
                                                       const char_type* end) {
      unsigned braces_counter{0u};
      for (; begin != end; ++begin) {
        if (*begin == '{') {
          ++braces_counter;
        } else if (*begin == '}') {
          if (braces_counter == 0u) {
            break;
          }
          --braces_counter;
        }
      }

      return begin;
    }

   private:
    unsigned counter_;
  };

  static FMT_CONSTEXPR unsigned count_parts() {
    FMT_CONSTEXPR_DECL const auto text = to_string_view(Format{});
    count_handler handler;
    internal::parse_format_string</*IS_CONSTEXPR=*/true>(text, handler);
    return handler.result();
  }

// Workaround for old compilers. Compiletime parts preparation will not be
// performed with them anyway.
#if FMT_USE_CONSTEXPR
  static FMT_CONSTEXPR_DECL const unsigned number_of_format_parts =
      compiletime_prepared_parts_type_provider::count_parts();
#else
  static const unsigned number_of_format_parts = 0u;
#endif

 public:
  template <unsigned N> struct format_parts_array {
    typedef format_part<char_type> value_type;

    FMT_CONSTEXPR format_parts_array() : arr{} {}

    FMT_CONSTEXPR value_type& operator[](unsigned ind) { return arr[ind]; }

    FMT_CONSTEXPR const value_type* begin() const { return arr; }

    FMT_CONSTEXPR const value_type* end() const { return begin() + N; }

   private:
    value_type arr[N];
  };

  struct empty {
    // Parts preparator will search for it
    typedef format_part<char_type> value_type;
  };

  typedef typename std::conditional<static_cast<bool>(number_of_format_parts),
                                    format_parts_array<number_of_format_parts>,
                                    empty>::type type;
};

template <typename Parts> class compiletime_prepared_parts_collector {
 private:
  typedef typename Parts::value_type format_part;

 public:
  FMT_CONSTEXPR explicit compiletime_prepared_parts_collector(Parts& parts)
      : parts_{parts}, counter_{0u} {}

  FMT_CONSTEXPR void add(format_part part) { parts_[counter_++] = part; }

  FMT_CONSTEXPR void substitute_last(format_part part) {
    parts_[counter_ - 1] = part;
  }

  FMT_CONSTEXPR format_part last() { return parts_[counter_ - 1]; }

 private:
  Parts& parts_;
  unsigned counter_;
};

template <typename PartsContainer, typename Char>
FMT_CONSTEXPR PartsContainer prepare_parts(basic_string_view<Char> format) {
  PartsContainer parts;
  internal::parse_format_string</*IS_CONSTEXPR=*/false>(
      format, format_preparation_handler<Char, PartsContainer>(format, parts));
  return parts;
}

template <typename PartsContainer, typename Char>
FMT_CONSTEXPR PartsContainer
prepare_compiletime_parts(basic_string_view<Char> format) {
  typedef compiletime_prepared_parts_collector<PartsContainer> collector;

  PartsContainer parts;
  collector c(parts);
  internal::parse_format_string</*IS_CONSTEXPR=*/true>(
      format, format_preparation_handler<Char, collector>(format, c));
  return parts;
}

template <typename PartsContainer> class runtime_parts_provider {
 public:
  runtime_parts_provider() = delete;
  template <typename Char>
  runtime_parts_provider(basic_string_view<Char> format)
      : parts_(prepare_parts<PartsContainer>(format)) {}

  const PartsContainer& parts() const { return parts_; }

 private:
  PartsContainer parts_;
};

template <typename Format, typename PartsContainer>
struct compiletime_parts_provider {
  compiletime_parts_provider() = delete;
  template <typename Char>
  FMT_CONSTEXPR compiletime_parts_provider(basic_string_view<Char>) {}

  const PartsContainer& parts() const {
    static FMT_CONSTEXPR_DECL const PartsContainer prepared_parts =
        prepare_compiletime_parts<PartsContainer>(
            internal::to_string_view(Format{}));

    return prepared_parts;
  }
};

template <typename PartsContainer>
struct parts_container_concept_check : std::true_type {
#if FMT_HAS_CONSTRUCTIBLE_TRAITS
  static_assert(std::is_copy_constructible<PartsContainer>::value,
                "PartsContainer is not copy constructible");
  static_assert(std::is_move_constructible<PartsContainer>::value,
                "PartsContainer is not move constructible");
#endif

  template <typename T, typename = void>
  struct has_format_part_type : std::false_type {};
  template <typename T>
  struct has_format_part_type<
      T, typename void_<typename T::format_part_type>::type> : std::true_type {
  };

  static_assert(has_format_part_type<PartsContainer>::value,
                "PartsContainer doesn't provide format_part_type typedef");

  struct check_second {};
  struct check_first : check_second {};

  template <typename T> static std::false_type has_add_check(check_second);
  template <typename T>
  static decltype(
      (void)declval<T>().add(declval<typename T::format_part_type>()),
      std::true_type()) has_add_check(check_first);
  typedef decltype(has_add_check<PartsContainer>(check_first())) has_add;
  static_assert(has_add::value, "PartsContainer doesn't provide add() method");

  template <typename T> static std::false_type has_last_check(check_second);
  template <typename T>
  static decltype((void)declval<T>().last(),
                  std::true_type()) has_last_check(check_first);
  typedef decltype(has_last_check<PartsContainer>(check_first())) has_last;
  static_assert(has_last::value,
                "PartsContainer doesn't provide last() method");

  template <typename T>
  static std::false_type has_substitute_last_check(check_second);
  template <typename T>
  static decltype((void)declval<T>().substitute_last(
                      declval<typename T::format_part_type>()),
                  std::true_type()) has_substitute_last_check(check_first);
  typedef decltype(has_substitute_last_check<PartsContainer>(
      check_first())) has_substitute_last;
  static_assert(has_substitute_last::value,
                "PartsContainer doesn't provide substitute_last() method");

  template <typename T> static std::false_type has_begin_check(check_second);
  template <typename T>
  static decltype((void)declval<T>().begin(),
                  std::true_type()) has_begin_check(check_first);
  typedef decltype(has_begin_check<PartsContainer>(check_first())) has_begin;
  static_assert(has_begin::value,
                "PartsContainer doesn't provide begin() method");

  template <typename T> static std::false_type has_end_check(check_second);
  template <typename T>
  static decltype((void)declval<T>().end(),
                  std::true_type()) has_end_check(check_first);
  typedef decltype(has_end_check<PartsContainer>(check_first())) has_end;
  static_assert(has_end::value, "PartsContainer doesn't provide end() method");
};

template <bool IS_CONSTEXPR, typename Format, typename /*PartsContainer*/>
struct parts_provider_type {
  typedef compiletime_parts_provider<
      Format, typename compiletime_prepared_parts_type_provider<Format>::type>
      type;
};

template <typename Format, typename PartsContainer>
struct parts_provider_type</*IS_CONSTEXPR=*/false, Format, PartsContainer> {
  static_assert(parts_container_concept_check<PartsContainer>::value,
                "Parts container doesn't meet the concept");
  typedef runtime_parts_provider<PartsContainer> type;
};

template <typename Format, typename PreparedPartsContainer, typename... Args>
struct basic_prepared_format {
  typedef internal::prepared_format<Format,
                                    typename internal::parts_provider_type<
                                        is_compile_string<Format>::value,
                                        Format, PreparedPartsContainer>::type,
                                    Args...>
      type;
};

template <typename Char>
std::basic_string<Char> to_runtime_format(basic_string_view<Char> format) {
  return std::basic_string<Char>(format.begin(), format.size());
}

template <typename Char>
std::basic_string<Char> to_runtime_format(const Char* format) {
  return std::basic_string<Char>(format);
}

template <typename Char, typename Container = std::vector<format_part<Char>>>
class parts_container {
 public:
  typedef format_part<Char> format_part_type;

  void add(format_part_type part) { parts_.push_back(std::move(part)); }

  void substitute_last(format_part_type part) {
    parts_.back() = std::move(part);
  }

  format_part_type last() { return parts_.back(); }

  auto begin() -> decltype(internal::declval<Container>().begin()) {
    return parts_.begin();
  }

  auto begin() const -> decltype(internal::declval<const Container>().begin()) {
    return parts_.begin();
  }

  auto end() -> decltype(internal::declval<Container>().end()) {
    return parts_.end();
  }

  auto end() const -> decltype(internal::declval<const Container>().end()) {
    return parts_.end();
  }

 private:
  Container parts_;
};

// Delegate preparing to preparator, to take advantage of a partial
// specialization.
template <typename Format, typename... Args> struct preparator {
  typedef parts_container<FMT_CHAR(Format)> container;
  typedef typename basic_prepared_format<Format, container, Args...>::type
      prepared_format_type;

  static auto prepare(Format format) -> prepared_format_type {
    return prepared_format_type(std::move(format));
  }
};

template <typename PassedFormat, typename PreparedFormatFormat,
          typename PartsContainer, typename... Args>
struct preparator<PassedFormat, prepared_format<PreparedFormatFormat,
                                                PartsContainer, Args...>> {
  typedef prepared_format<PreparedFormatFormat, PartsContainer, Args...>
      prepared_format_type;

  static auto prepare(PassedFormat format) -> prepared_format_type {
    return prepared_format_type(std::move(format));
  }
};

struct compiletime_format_tag {};
struct runtime_format_tag {};

template <typename Format> struct format_tag {
  typedef typename std::conditional<is_compile_string<Format>::value,
                                    compiletime_format_tag,
                                    runtime_format_tag>::type type;
};

#if FMT_USE_CONSTEXPR
template <typename Format, typename... Args>
auto do_prepare(runtime_format_tag, Format format) {
  return preparator<Format, Args...>::prepare(std::move(format));
}

template <typename Format, typename... Args>
FMT_CONSTEXPR auto do_prepare(compiletime_format_tag, const Format& format) {
  return typename basic_prepared_format<Format, void, Args...>::type(format);
}
#else
template <typename Format, typename... Args>
auto do_prepare(const Format& format)
    -> decltype(preparator<Format, Args...>::prepare(format)) {
  return preparator<Format, Args...>::prepare(format);
}
#endif
}  // namespace internal

template <typename Char, typename Container = std::vector<format_part<Char>>>
struct parts_container {
  typedef internal::parts_container<Char, Container> type;
};

template <typename Format, typename PartsContainer, typename... Args>
struct basic_prepared_format {
  typedef typename internal::basic_prepared_format<Format, PartsContainer,
                                                   Args...>::type type;
};

template <typename... Args> struct prepared_format {
  typedef typename basic_prepared_format<
      std::string, typename parts_container<char>::type, Args...>::type type;
};

template <typename... Args> struct wprepared_format {
  typedef
      typename basic_prepared_format<std::wstring,
                                     typename parts_container<wchar_t>::type,
                                     Args...>::type type;
};

#if FMT_USE_ALIAS_TEMPLATES

template <typename Char, typename Container = std::vector<format_part<Char>>>
using parts_container_t = typename parts_container<Char, Container>::type;

template <typename Format, typename PreparedPartsContainer, typename... Args>
using basic_prepared_format_t =
    typename basic_prepared_format<Format, PreparedPartsContainer,
                                   Args...>::type;

template <typename... Args>
using prepared_format_t =
    basic_prepared_format_t<std::string, parts_container<char>, Args...>;

template <typename... Args>
using wprepared_format_t =
    basic_prepared_format_t<std::wstring, parts_container<wchar_t>, Args...>;

#endif

#if FMT_USE_CONSTEXPR

template <typename... Args, typename Format>
FMT_CONSTEXPR auto prepare(Format format) {
  return internal::do_prepare<Format, Args...>(
      typename internal::format_tag<Format>::type{}, std::move(format));
}
#else

template <typename... Args, typename Format>
auto prepare(Format format) ->
    typename internal::preparator<Format, Args...>::prepared_format_type {
  return internal::preparator<Format, Args...>::prepare(std::move(format));
}
#endif

template <typename... Args, typename Char>
auto prepare(const Char* format) ->
    typename internal::preparator<std::basic_string<Char>,
                                  Args...>::prepared_format_type {
  return prepare<Args...>(internal::to_runtime_format(format));
}

template <typename... Args, typename Char, unsigned N>
auto prepare(const Char(format)[N]) ->
    typename internal::preparator<std::basic_string<Char>,
                                  Args...>::prepared_format_type {
  const auto view = basic_string_view<Char>(format, N);
  return prepare<Args...>(internal::to_runtime_format(view));
}

template <typename... Args, typename Char>
auto prepare(basic_string_view<Char> format) ->
    typename internal::preparator<std::basic_string<Char>,
                                  Args...>::prepared_format_type {
  return prepare<Args...>(internal::to_runtime_format(format));
}

FMT_END_NAMESPACE

#endif  // FMT_PREPARE_H_
