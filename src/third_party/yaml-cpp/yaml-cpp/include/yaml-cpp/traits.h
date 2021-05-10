#ifndef TRAITS_H_62B23520_7C8E_11DE_8A39_0800200C9A66
#define TRAITS_H_62B23520_7C8E_11DE_8A39_0800200C9A66

#if defined(_MSC_VER) ||                                            \
    (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || \
     (__GNUC__ >= 4))  // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#include <type_traits>
#include <utility>
#include <string>
#include <sstream>

namespace YAML {
template <typename>
struct is_numeric {
  enum { value = false };
};

template <>
struct is_numeric<char> {
  enum { value = true };
};
template <>
struct is_numeric<unsigned char> {
  enum { value = true };
};
template <>
struct is_numeric<int> {
  enum { value = true };
};
template <>
struct is_numeric<unsigned int> {
  enum { value = true };
};
template <>
struct is_numeric<long int> {
  enum { value = true };
};
template <>
struct is_numeric<unsigned long int> {
  enum { value = true };
};
template <>
struct is_numeric<short int> {
  enum { value = true };
};
template <>
struct is_numeric<unsigned short int> {
  enum { value = true };
};
#if defined(_MSC_VER) && (_MSC_VER < 1310)
template <>
struct is_numeric<__int64> {
  enum { value = true };
};
template <>
struct is_numeric<unsigned __int64> {
  enum { value = true };
};
#else
template <>
struct is_numeric<long long> {
  enum { value = true };
};
template <>
struct is_numeric<unsigned long long> {
  enum { value = true };
};
#endif
template <>
struct is_numeric<float> {
  enum { value = true };
};
template <>
struct is_numeric<double> {
  enum { value = true };
};
template <>
struct is_numeric<long double> {
  enum { value = true };
};

template <bool, class T = void>
struct enable_if_c {
  typedef T type;
};

template <class T>
struct enable_if_c<false, T> {};

template <class Cond, class T = void>
struct enable_if : public enable_if_c<Cond::value, T> {};

template <bool, class T = void>
struct disable_if_c {
  typedef T type;
};

template <class T>
struct disable_if_c<true, T> {};

template <class Cond, class T = void>
struct disable_if : public disable_if_c<Cond::value, T> {};
}

template <typename S, typename T>
struct is_streamable {
  template <typename SS, typename TT>
  static auto test(int)
      -> decltype(std::declval<SS&>() << std::declval<TT>(), std::true_type());

  template <typename, typename>
  static auto test(...) -> std::false_type;

  static const bool value = decltype(test<S, T>(0))::value;
};

template<typename Key, bool Streamable>
struct streamable_to_string {
  static std::string impl(const Key& key) {
    std::stringstream ss;
    ss << key;
    return ss.str();
  }
};

template<typename Key>
struct streamable_to_string<Key, false> {
  static std::string impl(const Key&) {
    return "";
  }
};
#endif  // TRAITS_H_62B23520_7C8E_11DE_8A39_0800200C9A66
