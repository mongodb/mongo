///////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2015 Microsoft Corporation. All rights reserved.
//
// This code is licensed under the MIT License (MIT).
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
///////////////////////////////////////////////////////////////////////////////

// Adapted from
// https://github.com/Microsoft/GSL/blob/3819df6e378ffccf0e29465afe99c3b324c2aa70/include/gsl/span
// and
// https://github.com/Microsoft/GSL/blob/3819df6e378ffccf0e29465afe99c3b324c2aa70/include/gsl/gsl_util

#ifndef mozilla_Span_h
#define mozilla_Span_h

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/UniquePtr.h"

namespace mozilla {

template <typename T, size_t Length>
class Array;

template <typename Enum, typename T, size_t Length>
class EnumeratedArray;

// Stuff from gsl_util

// narrow_cast(): a searchable way to do narrowing casts of values
template <class T, class U>
inline constexpr T narrow_cast(U&& u) {
  return static_cast<T>(std::forward<U>(u));
}

// end gsl_util

// [views.constants], constants
// This was -1 in gsl::span, but using size_t for sizes instead of ptrdiff_t
// and reserving a magic value that realistically doesn't occur in
// compile-time-constant Span sizes makes things a lot less messy in terms of
// comparison between signed and unsigned.
constexpr const size_t dynamic_extent = std::numeric_limits<size_t>::max();

template <class ElementType, size_t Extent = dynamic_extent>
class Span;

// implementation details
namespace span_details {

template <class T>
struct is_span_oracle : std::false_type {};

template <class ElementType, size_t Extent>
struct is_span_oracle<mozilla::Span<ElementType, Extent>> : std::true_type {};

template <class T>
struct is_span : public is_span_oracle<std::remove_cv_t<T>> {};

template <class T>
struct is_std_array_oracle : std::false_type {};

template <class ElementType, size_t Extent>
struct is_std_array_oracle<std::array<ElementType, Extent>> : std::true_type {};

template <class T>
struct is_std_array : public is_std_array_oracle<std::remove_cv_t<T>> {};

template <size_t From, size_t To>
struct is_allowed_extent_conversion
    : public std::integral_constant<bool, From == To ||
                                              From == mozilla::dynamic_extent ||
                                              To == mozilla::dynamic_extent> {};

template <class From, class To>
struct is_allowed_element_type_conversion
    : public std::integral_constant<
          bool, std::is_convertible_v<From (*)[], To (*)[]>> {};

struct SpanKnownBounds {};

template <class SpanT, bool IsConst>
class span_iterator {
  using element_type_ = typename SpanT::element_type;

  template <class ElementType, size_t Extent>
  friend class ::mozilla::Span;

 public:
  using iterator_category = std::random_access_iterator_tag;
  using value_type = std::remove_const_t<element_type_>;
  using difference_type = ptrdiff_t;

  using reference =
      std::conditional_t<IsConst, const element_type_, element_type_>&;
  using pointer = std::add_pointer_t<reference>;

  constexpr span_iterator() : span_iterator(nullptr, 0, SpanKnownBounds{}) {}

  constexpr span_iterator(const SpanT* span, typename SpanT::index_type index)
      : span_(span), index_(index) {
    MOZ_RELEASE_ASSERT(span == nullptr ||
                       (index_ >= 0 && index <= span_->Length()));
  }

 private:
  // For whatever reason, the compiler doesn't like optimizing away the above
  // MOZ_RELEASE_ASSERT when `span_iterator` is constructed for
  // obviously-correct cases like `span.begin()` or `span.end()`.  We provide
  // this private constructor for such cases.
  constexpr span_iterator(const SpanT* span, typename SpanT::index_type index,
                          SpanKnownBounds)
      : span_(span), index_(index) {}

 public:
  // `other` is already correct by construction; we do not need to go through
  // the release assert above.  Put differently, this constructor is effectively
  // a copy constructor and therefore needs no assertions.
  friend class span_iterator<SpanT, true>;
  constexpr MOZ_IMPLICIT span_iterator(const span_iterator<SpanT, false>& other)
      : span_(other.span_), index_(other.index_) {}

  constexpr span_iterator<SpanT, IsConst>& operator=(
      const span_iterator<SpanT, IsConst>&) = default;

  constexpr reference operator*() const {
    MOZ_RELEASE_ASSERT(span_);
    return (*span_)[index_];
  }

  constexpr pointer operator->() const {
    MOZ_RELEASE_ASSERT(span_);
    return &((*span_)[index_]);
  }

  constexpr span_iterator& operator++() {
    ++index_;
    return *this;
  }

  constexpr span_iterator operator++(int) {
    auto ret = *this;
    ++(*this);
    return ret;
  }

  constexpr span_iterator& operator--() {
    --index_;
    return *this;
  }

  constexpr span_iterator operator--(int) {
    auto ret = *this;
    --(*this);
    return ret;
  }

  constexpr span_iterator operator+(difference_type n) const {
    auto ret = *this;
    return ret += n;
  }

  constexpr span_iterator& operator+=(difference_type n) {
    MOZ_RELEASE_ASSERT(span_ && (index_ + n) >= 0 &&
                       (index_ + n) <= span_->Length());
    index_ += n;
    return *this;
  }

  constexpr span_iterator operator-(difference_type n) const {
    auto ret = *this;
    return ret -= n;
  }

  constexpr span_iterator& operator-=(difference_type n) { return *this += -n; }

  constexpr difference_type operator-(const span_iterator& rhs) const {
    MOZ_RELEASE_ASSERT(span_ == rhs.span_);
    return index_ - rhs.index_;
  }

  constexpr reference operator[](difference_type n) const {
    return *(*this + n);
  }

  constexpr friend bool operator==(const span_iterator& lhs,
                                   const span_iterator& rhs) {
    // Iterators from different spans are uncomparable. A diagnostic assertion
    // should be enough to check this, though. To ensure that no iterators from
    // different spans are ever considered equal, still compare them in release
    // builds.
    MOZ_DIAGNOSTIC_ASSERT(lhs.span_ == rhs.span_);
    return lhs.index_ == rhs.index_ && lhs.span_ == rhs.span_;
  }

  constexpr friend bool operator!=(const span_iterator& lhs,
                                   const span_iterator& rhs) {
    return !(lhs == rhs);
  }

  constexpr friend bool operator<(const span_iterator& lhs,
                                  const span_iterator& rhs) {
    MOZ_DIAGNOSTIC_ASSERT(lhs.span_ == rhs.span_);
    return lhs.index_ < rhs.index_;
  }

  constexpr friend bool operator<=(const span_iterator& lhs,
                                   const span_iterator& rhs) {
    return !(rhs < lhs);
  }

  constexpr friend bool operator>(const span_iterator& lhs,
                                  const span_iterator& rhs) {
    return rhs < lhs;
  }

  constexpr friend bool operator>=(const span_iterator& lhs,
                                   const span_iterator& rhs) {
    return !(rhs > lhs);
  }

  void swap(span_iterator& rhs) {
    std::swap(index_, rhs.index_);
    std::swap(span_, rhs.span_);
  }

 protected:
  const SpanT* span_;
  size_t index_;
};

template <class Span, bool IsConst>
inline constexpr span_iterator<Span, IsConst> operator+(
    typename span_iterator<Span, IsConst>::difference_type n,
    const span_iterator<Span, IsConst>& rhs) {
  return rhs + n;
}

template <size_t Ext>
class extent_type {
 public:
  using index_type = size_t;

  static_assert(Ext >= 0, "A fixed-size Span must be >= 0 in size.");

  constexpr extent_type() = default;

  template <index_type Other>
  constexpr MOZ_IMPLICIT extent_type(extent_type<Other> ext) {
    static_assert(
        Other == Ext || Other == dynamic_extent,
        "Mismatch between fixed-size extent and size of initializing data.");
    MOZ_RELEASE_ASSERT(ext.size() == Ext);
  }

  constexpr MOZ_IMPLICIT extent_type(index_type length) {
    MOZ_RELEASE_ASSERT(length == Ext);
  }

  constexpr index_type size() const { return Ext; }
};

template <>
class extent_type<dynamic_extent> {
 public:
  using index_type = size_t;

  template <index_type Other>
  explicit constexpr extent_type(extent_type<Other> ext) : size_(ext.size()) {}

  explicit constexpr extent_type(index_type length) : size_(length) {}

  constexpr index_type size() const { return size_; }

 private:
  index_type size_;
};
}  // namespace span_details

/**
 * Span - slices for C++
 *
 * Span implements Rust's slice concept for C++. It's called "Span" instead of
 * "Slice" to follow the naming used in C++ Core Guidelines.
 *
 * A Span wraps a pointer and a length that identify a non-owning view to a
 * contiguous block of memory of objects of the same type. Various types,
 * including (pre-decay) C arrays, XPCOM strings, nsTArray, mozilla::Array,
 * mozilla::Range and contiguous standard-library containers, auto-convert
 * into Spans when attempting to pass them as arguments to methods that take
 * Spans. (Span itself autoconverts into mozilla::Range.)
 *
 * Like Rust's slices, Span provides safety against out-of-bounds access by
 * performing run-time bound checks. However, unlike Rust's slices, Span
 * cannot provide safety against use-after-free.
 *
 * (Note: Span is like Rust's slice only conceptually. Due to the lack of
 * ABI guarantees, you should still decompose spans/slices to raw pointer
 * and length parts when crossing the FFI. The Elements() and data() methods
 * are guaranteed to return a non-null pointer even for zero-length spans,
 * so the pointer can be used as a raw part of a Rust slice without further
 * checks.)
 *
 * In addition to having constructors (with the support of deduction guides)
 * that take various well-known types, a Span for an arbitrary type can be
 * constructed from a pointer and a length or a pointer and another pointer
 * pointing just past the last element.
 *
 * A Span<const char> or Span<const char16_t> can be obtained for const char*
 * or const char16_t pointing to a zero-terminated string using the
 * MakeStringSpan() function (which treats a nullptr argument equivalently
 * to the empty string). Corresponding implicit constructor does not exist
 * in order to avoid accidental construction in cases where const char* or
 * const char16_t* do not point to a zero-terminated string.
 *
 * Span has methods that follow the Mozilla naming style and methods that
 * don't. The methods that follow the Mozilla naming style are meant to be
 * used directly from Mozilla code. The methods that don't are meant for
 * integration with C++11 range-based loops and with meta-programming that
 * expects the same methods that are found on the standard-library
 * containers. For example, to decompose a Span into its parts in Mozilla
 * code, use Elements() and Length() (as with nsTArray) instead of data()
 * and size() (as with std::vector).
 *
 * The pointer and length wrapped by a Span cannot be changed after a Span has
 * been created. When new values are required, simply create a new Span. Span
 * has a method called Subspan() that works analogously to the Substring()
 * method of XPCOM strings taking a start index and an optional length. As a
 * Mozilla extension (relative to Microsoft's gsl::span that mozilla::Span is
 * based on), Span has methods From(start), To(end) and FromTo(start, end)
 * that correspond to Rust's &slice[start..], &slice[..end] and
 * &slice[start..end], respectively. (That is, the end index is the index of
 * the first element not to be included in the new subspan.)
 *
 * When indicating a Span that's only read from, const goes inside the type
 * parameter. Don't put const in front of Span. That is:
 * size_t ReadsFromOneSpanAndWritesToAnother(Span<const uint8_t> aReadFrom,
 *                                           Span<uint8_t> aWrittenTo);
 *
 * Any Span<const T> can be viewed as Span<const uint8_t> using the function
 * AsBytes(). Any Span<T> can be viewed as Span<uint8_t> using the function
 * AsWritableBytes().
 *
 * Note that iterators from different Span instances are uncomparable, even if
 * they refer to the same memory. This also applies to any spans derived via
 * Subspan etc.
 */
template <class ElementType, size_t Extent /* = dynamic_extent */>
class Span {
 public:
  // constants and types
  using element_type = ElementType;
  using value_type = std::remove_cv_t<element_type>;
  using index_type = size_t;
  using pointer = element_type*;
  using reference = element_type&;

  using iterator =
      span_details::span_iterator<Span<ElementType, Extent>, false>;
  using const_iterator =
      span_details::span_iterator<Span<ElementType, Extent>, true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  constexpr static const index_type extent = Extent;

  // [Span.cons], Span constructors, copy, assignment, and destructor
  // "Dependent" is needed to make "std::enable_if_t<(Dependent ||
  //   Extent == 0 || Extent == dynamic_extent)>" SFINAE,
  // since
  // "std::enable_if_t<(Extent == 0 || Extent == dynamic_extent)>" is
  // ill-formed when Extent is neither of the extreme values.
  /**
   * Constructor with no args.
   */
  template <bool Dependent = false,
            class = std::enable_if_t<(Dependent || Extent == 0 ||
                                      Extent == dynamic_extent)>>
  constexpr Span() : storage_(nullptr, span_details::extent_type<0>()) {}

  /**
   * Constructor for nullptr.
   */
  constexpr MOZ_IMPLICIT Span(std::nullptr_t) : Span() {}

  /**
   * Constructor for pointer and length.
   */
  constexpr Span(pointer aPtr, index_type aLength) : storage_(aPtr, aLength) {}

  /**
   * Constructor for start pointer and pointer past end.
   */
  constexpr Span(pointer aStartPtr, pointer aEndPtr)
      : storage_(aStartPtr, std::distance(aStartPtr, aEndPtr)) {}

  /**
   * Constructor for pair of Span iterators.
   */
  template <typename OtherElementType, size_t OtherExtent, bool IsConst>
  constexpr Span(
      span_details::span_iterator<Span<OtherElementType, OtherExtent>, IsConst>
          aBegin,
      span_details::span_iterator<Span<OtherElementType, OtherExtent>, IsConst>
          aEnd)
      : storage_(aBegin == aEnd ? nullptr : &*aBegin, aEnd - aBegin) {}

  /**
   * Constructor for {iterator,size_t}
   */
  template <typename OtherElementType, size_t OtherExtent, bool IsConst>
  constexpr Span(
      span_details::span_iterator<Span<OtherElementType, OtherExtent>, IsConst>
          aBegin,
      index_type aLength)
      : storage_(!aLength ? nullptr : &*aBegin, aLength) {}

  /**
   * Constructor for C array.
   */
  template <size_t N>
  constexpr MOZ_IMPLICIT Span(element_type (&aArr)[N])
      : storage_(&aArr[0], span_details::extent_type<N>()) {}

  // Implicit constructors for char* and char16_t* pointers are deleted in order
  // to avoid accidental construction in cases where a pointer does not point to
  // a zero-terminated string. A Span<const char> or Span<const char16_t> can be
  // obtained for const char* or const char16_t pointing to a zero-terminated
  // string using the MakeStringSpan() function.
  // (This must be a template because otherwise it will prevent the previous
  // array constructor to match because an array decays to a pointer. This only
  // exists to point to the above explanation, since there's no other
  // constructor that would match.)
  template <
      typename T,
      typename = std::enable_if_t<
          std::is_pointer_v<T> &&
          (std::is_same_v<std::remove_const_t<std::decay_t<T>>, char> ||
           std::is_same_v<std::remove_const_t<std::decay_t<T>>, char16_t>)>>
  Span(T& aStr) = delete;

  /**
   * Constructor for std::array.
   */
  template <size_t N,
            class ArrayElementType = std::remove_const_t<element_type>>
  constexpr MOZ_IMPLICIT Span(std::array<ArrayElementType, N>& aArr)
      : storage_(&aArr[0], span_details::extent_type<N>()) {}

  /**
   * Constructor for const std::array.
   */
  template <size_t N>
  constexpr MOZ_IMPLICIT Span(
      const std::array<std::remove_const_t<element_type>, N>& aArr)
      : storage_(&aArr[0], span_details::extent_type<N>()) {}

  /**
   * Constructor for mozilla::Array.
   */
  template <size_t N,
            class ArrayElementType = std::remove_const_t<element_type>>
  constexpr MOZ_IMPLICIT Span(mozilla::Array<ArrayElementType, N>& aArr)
      : storage_(&aArr[0], span_details::extent_type<N>()) {}

  /**
   * Constructor for const mozilla::Array.
   */
  template <size_t N>
  constexpr MOZ_IMPLICIT Span(
      const mozilla::Array<std::remove_const_t<element_type>, N>& aArr)
      : storage_(&aArr[0], span_details::extent_type<N>()) {}

  /**
   * Constructor for mozilla::EnumeratedArray.
   */
  template <size_t N, class Enum,
            class ArrayElementType = std::remove_const_t<element_type>>
  constexpr MOZ_IMPLICIT Span(
      mozilla::EnumeratedArray<Enum, ArrayElementType, N>& aArr)
      : storage_(&aArr[Enum(0)], span_details::extent_type<N>()) {}

  /**
   * Constructor for const mozilla::EnumeratedArray.
   */
  template <size_t N, class Enum>
  constexpr MOZ_IMPLICIT Span(const mozilla::EnumeratedArray<
                              Enum, std::remove_const_t<element_type>, N>& aArr)
      : storage_(&aArr[Enum(0)], span_details::extent_type<N>()) {}

  /**
   * Constructor for mozilla::UniquePtr holding an array and length.
   */
  template <class ArrayElementType = std::add_pointer<element_type>,
            class DeleterType>
  constexpr Span(const mozilla::UniquePtr<ArrayElementType, DeleterType>& aPtr,
                 index_type aLength)
      : storage_(aPtr.get(), aLength) {}

  // NB: the SFINAE here uses .data() as a incomplete/imperfect proxy for the
  // requirement on Container to be a contiguous sequence container.
  /**
   * Constructor for standard-library containers.
   */
  template <
      class Container,
      class Dummy = std::enable_if_t<
          !std::is_const_v<Container> &&
              !span_details::is_span<Container>::value &&
              !span_details::is_std_array<Container>::value &&
              std::is_convertible_v<typename Container::pointer, pointer> &&
              std::is_convertible_v<typename Container::pointer,
                                    decltype(std::declval<Container>().data())>,
          Container>>
  constexpr MOZ_IMPLICIT Span(Container& cont, Dummy* = nullptr)
      : Span(cont.data(), ReleaseAssertedCast<index_type>(cont.size())) {}

  /**
   * Constructor for standard-library containers (const version).
   */
  template <
      class Container,
      class = std::enable_if_t<
          std::is_const_v<element_type> &&
          !span_details::is_span<Container>::value &&
          std::is_convertible_v<typename Container::pointer, pointer> &&
          std::is_convertible_v<typename Container::pointer,
                                decltype(std::declval<Container>().data())>>>
  constexpr MOZ_IMPLICIT Span(const Container& cont)
      : Span(cont.data(), ReleaseAssertedCast<index_type>(cont.size())) {}

  // NB: the SFINAE here uses .Elements() as a incomplete/imperfect proxy for
  // the requirement on Container to be a contiguous sequence container.
  /**
   * Constructor for contiguous Mozilla containers.
   */
  template <
      class Container,
      class = std::enable_if_t<
          !std::is_const_v<Container> &&
          !span_details::is_span<Container>::value &&
          !span_details::is_std_array<Container>::value &&
          std::is_convertible_v<typename Container::value_type*, pointer> &&
          std::is_convertible_v<
              typename Container::value_type*,
              decltype(std::declval<Container>().Elements())>>>
  constexpr MOZ_IMPLICIT Span(Container& cont, void* = nullptr)
      : Span(cont.Elements(), ReleaseAssertedCast<index_type>(cont.Length())) {}

  /**
   * Constructor for contiguous Mozilla containers (const version).
   */
  template <
      class Container,
      class = std::enable_if_t<
          std::is_const_v<element_type> &&
          !span_details::is_span<Container>::value &&
          std::is_convertible_v<typename Container::value_type*, pointer> &&
          std::is_convertible_v<
              typename Container::value_type*,
              decltype(std::declval<Container>().Elements())>>>
  constexpr MOZ_IMPLICIT Span(const Container& cont, void* = nullptr)
      : Span(cont.Elements(), ReleaseAssertedCast<index_type>(cont.Length())) {}

  /**
   * Constructor from other Span.
   */
  constexpr Span(const Span& other) = default;

  /**
   * Constructor from other Span.
   */
  constexpr Span(Span&& other) = default;

  /**
   * Constructor from other Span with conversion of element type.
   */
  template <
      class OtherElementType, size_t OtherExtent,
      class = std::enable_if_t<span_details::is_allowed_extent_conversion<
                                   OtherExtent, Extent>::value &&
                               span_details::is_allowed_element_type_conversion<
                                   OtherElementType, element_type>::value>>
  constexpr MOZ_IMPLICIT Span(const Span<OtherElementType, OtherExtent>& other)
      : storage_(other.data(),
                 span_details::extent_type<OtherExtent>(other.size())) {}

  /**
   * Constructor from other Span with conversion of element type.
   */
  template <
      class OtherElementType, size_t OtherExtent,
      class = std::enable_if_t<span_details::is_allowed_extent_conversion<
                                   OtherExtent, Extent>::value &&
                               span_details::is_allowed_element_type_conversion<
                                   OtherElementType, element_type>::value>>
  constexpr MOZ_IMPLICIT Span(Span<OtherElementType, OtherExtent>&& other)
      : storage_(other.data(),
                 span_details::extent_type<OtherExtent>(other.size())) {}

  ~Span() = default;
  constexpr Span& operator=(const Span& other) = default;

  constexpr Span& operator=(Span&& other) = default;

  // [Span.sub], Span subviews
  /**
   * Subspan with first N elements with compile-time N.
   */
  template <size_t Count>
  constexpr Span<element_type, Count> First() const {
    MOZ_RELEASE_ASSERT(Count <= size());
    return {data(), Count};
  }

  /**
   * Subspan with last N elements with compile-time N.
   */
  template <size_t Count>
  constexpr Span<element_type, Count> Last() const {
    const size_t len = size();
    MOZ_RELEASE_ASSERT(Count <= len);
    return {data() + (len - Count), Count};
  }

  /**
   * Subspan with compile-time start index and length.
   */
  template <size_t Offset, size_t Count = dynamic_extent>
  constexpr Span<element_type, Count> Subspan() const {
    const size_t len = size();
    MOZ_RELEASE_ASSERT(Offset <= len &&
                       (Count == dynamic_extent || (Offset + Count <= len)));
    return {data() + Offset, Count == dynamic_extent ? len - Offset : Count};
  }

  /**
   * Subspan with first N elements with run-time N.
   */
  constexpr Span<element_type, dynamic_extent> First(index_type aCount) const {
    MOZ_RELEASE_ASSERT(aCount <= size());
    return {data(), aCount};
  }

  /**
   * Subspan with last N elements with run-time N.
   */
  constexpr Span<element_type, dynamic_extent> Last(index_type aCount) const {
    const size_t len = size();
    MOZ_RELEASE_ASSERT(aCount <= len);
    return {data() + (len - aCount), aCount};
  }

  /**
   * Subspan with run-time start index and length.
   */
  constexpr Span<element_type, dynamic_extent> Subspan(
      index_type aStart, index_type aLength = dynamic_extent) const {
    const size_t len = size();
    MOZ_RELEASE_ASSERT(aStart <= len && (aLength == dynamic_extent ||
                                         (aStart + aLength <= len)));
    return {data() + aStart,
            aLength == dynamic_extent ? len - aStart : aLength};
  }

  /**
   * Subspan with run-time start index. (Rust's &foo[start..])
   */
  constexpr Span<element_type, dynamic_extent> From(index_type aStart) const {
    return Subspan(aStart);
  }

  /**
   * Subspan with run-time exclusive end index. (Rust's &foo[..end])
   */
  constexpr Span<element_type, dynamic_extent> To(index_type aEnd) const {
    return Subspan(0, aEnd);
  }

  /// std::span-compatible method name
  constexpr auto subspan(index_type aStart,
                         index_type aLength = dynamic_extent) const {
    return Subspan(aStart, aLength);
  }
  /// std::span-compatible method name
  constexpr auto from(index_type aStart) const { return From(aStart); }
  /// std::span-compatible method name
  constexpr auto to(index_type aEnd) const { return To(aEnd); }

  /**
   * Subspan with run-time start index and exclusive end index.
   * (Rust's &foo[start..end])
   */
  constexpr Span<element_type, dynamic_extent> FromTo(index_type aStart,
                                                      index_type aEnd) const {
    MOZ_RELEASE_ASSERT(aStart <= aEnd);
    return Subspan(aStart, aEnd - aStart);
  }

  // [Span.obs], Span observers
  /**
   * Number of elements in the span.
   */
  constexpr index_type Length() const { return size(); }

  /**
   * Number of elements in the span (standard-libray duck typing version).
   */
  constexpr index_type size() const { return storage_.size(); }

  /**
   * Size of the span in bytes.
   */
  constexpr index_type LengthBytes() const { return size_bytes(); }

  /**
   * Size of the span in bytes (standard-library naming style version).
   */
  constexpr index_type size_bytes() const {
    return size() * narrow_cast<index_type>(sizeof(element_type));
  }

  /**
   * Checks if the the length of the span is zero.
   */
  constexpr bool IsEmpty() const { return empty(); }

  /**
   * Checks if the the length of the span is zero (standard-libray duck
   * typing version).
   */
  constexpr bool empty() const { return size() == 0; }

  // [Span.elem], Span element access
  constexpr reference operator[](index_type idx) const {
    MOZ_RELEASE_ASSERT(idx < storage_.size());
    return data()[idx];
  }

  /**
   * Access element of span by index (standard-library duck typing version).
   */
  constexpr reference at(index_type idx) const { return this->operator[](idx); }

  constexpr reference operator()(index_type idx) const {
    return this->operator[](idx);
  }

  /**
   * Pointer to the first element of the span. The return value is never
   * nullptr, not ever for zero-length spans, so it can be passed as-is
   * to std::slice::from_raw_parts() in Rust.
   */
  constexpr pointer Elements() const { return data(); }

  /**
   * Pointer to the first element of the span (standard-libray duck typing
   * version). The return value is never nullptr, not ever for zero-length
   * spans, so it can be passed as-is to std::slice::from_raw_parts() in Rust.
   */
  constexpr pointer data() const { return storage_.data(); }

  // [Span.iter], Span iterator support
  iterator begin() const { return {this, 0, span_details::SpanKnownBounds{}}; }
  iterator end() const {
    return {this, Length(), span_details::SpanKnownBounds{}};
  }

  const_iterator cbegin() const {
    return {this, 0, span_details::SpanKnownBounds{}};
  }
  const_iterator cend() const {
    return {this, Length(), span_details::SpanKnownBounds{}};
  }

  reverse_iterator rbegin() const { return reverse_iterator{end()}; }
  reverse_iterator rend() const { return reverse_iterator{begin()}; }

  const_reverse_iterator crbegin() const {
    return const_reverse_iterator{cend()};
  }
  const_reverse_iterator crend() const {
    return const_reverse_iterator{cbegin()};
  }

  template <size_t SplitPoint>
  constexpr std::pair<Span<ElementType, SplitPoint>,
                      Span<ElementType, Extent - SplitPoint>>
  SplitAt() const {
    static_assert(Extent != dynamic_extent);
    static_assert(SplitPoint <= Extent);
    return {First<SplitPoint>(), Last<Extent - SplitPoint>()};
  }

  constexpr std::pair<Span<ElementType, dynamic_extent>,
                      Span<ElementType, dynamic_extent>>
  SplitAt(const index_type aSplitPoint) const {
    MOZ_RELEASE_ASSERT(aSplitPoint <= Length());
    return {First(aSplitPoint), Last(Length() - aSplitPoint)};
  }

  constexpr Span<std::add_const_t<ElementType>, Extent> AsConst() const {
    return {Elements(), Length()};
  }

 private:
  // this implementation detail class lets us take advantage of the
  // empty base class optimization to pay for only storage of a single
  // pointer in the case of fixed-size Spans
  template <class ExtentType>
  class storage_type : public ExtentType {
   public:
    template <class OtherExtentType>
    constexpr storage_type(pointer elements, OtherExtentType ext)
        : ExtentType(ext)
          // Replace nullptr with aligned bogus pointer for Rust slice
          // compatibility. See
          // https://doc.rust-lang.org/std/slice/fn.from_raw_parts.html
          ,
          data_(elements ? elements
                         : reinterpret_cast<pointer>(alignof(element_type))) {
      const size_t extentSize = ExtentType::size();
      MOZ_RELEASE_ASSERT((!elements && extentSize == 0) ||
                         (elements && extentSize != dynamic_extent));
    }

    constexpr pointer data() const { return data_; }

   private:
    pointer data_;
  };

  storage_type<span_details::extent_type<Extent>> storage_;
};

template <typename T, size_t OtherExtent, bool IsConst>
Span(span_details::span_iterator<Span<T, OtherExtent>, IsConst> aBegin,
     span_details::span_iterator<Span<T, OtherExtent>, IsConst> aEnd)
    -> Span<std::conditional_t<IsConst, std::add_const_t<T>, T>>;

template <typename T, size_t Extent>
Span(T (&)[Extent]) -> Span<T, Extent>;

template <class Container>
Span(Container&) -> Span<typename Container::value_type>;

template <class Container>
Span(const Container&) -> Span<const typename Container::value_type>;

template <typename T, size_t Extent>
Span(mozilla::Array<T, Extent>&) -> Span<T, Extent>;

template <typename T, size_t Extent>
Span(const mozilla::Array<T, Extent>&) -> Span<const T, Extent>;

template <typename Enum, typename T, size_t Extent>
Span(mozilla::EnumeratedArray<Enum, T, Extent>&) -> Span<T, Extent>;

template <typename Enum, typename T, size_t Extent>
Span(const mozilla::EnumeratedArray<Enum, T, Extent>&) -> Span<const T, Extent>;

// [Span.comparison], Span comparison operators
template <class ElementType, size_t FirstExtent, size_t SecondExtent>
inline constexpr bool operator==(const Span<ElementType, FirstExtent>& l,
                                 const Span<ElementType, SecondExtent>& r) {
  return (l.size() == r.size()) &&
         std::equal(l.data(), l.data() + l.size(), r.data());
}

template <class ElementType, size_t Extent>
inline constexpr bool operator!=(const Span<ElementType, Extent>& l,
                                 const Span<ElementType, Extent>& r) {
  return !(l == r);
}

template <class ElementType, size_t Extent>
inline constexpr bool operator<(const Span<ElementType, Extent>& l,
                                const Span<ElementType, Extent>& r) {
  return std::lexicographical_compare(l.data(), l.data() + l.size(), r.data(),
                                      r.data() + r.size());
}

template <class ElementType, size_t Extent>
inline constexpr bool operator<=(const Span<ElementType, Extent>& l,
                                 const Span<ElementType, Extent>& r) {
  return !(l > r);
}

template <class ElementType, size_t Extent>
inline constexpr bool operator>(const Span<ElementType, Extent>& l,
                                const Span<ElementType, Extent>& r) {
  return r < l;
}

template <class ElementType, size_t Extent>
inline constexpr bool operator>=(const Span<ElementType, Extent>& l,
                                 const Span<ElementType, Extent>& r) {
  return !(l < r);
}

namespace span_details {
// if we only supported compilers with good constexpr support then
// this pair of classes could collapse down to a constexpr function

// we should use a narrow_cast<> to go to size_t, but older compilers may not
// see it as constexpr and so will fail compilation of the template
template <class ElementType, size_t Extent>
struct calculate_byte_size
    : std::integral_constant<size_t,
                             static_cast<size_t>(sizeof(ElementType) *
                                                 static_cast<size_t>(Extent))> {
};

template <class ElementType>
struct calculate_byte_size<ElementType, dynamic_extent>
    : std::integral_constant<size_t, dynamic_extent> {};
}  // namespace span_details

// [Span.objectrep], views of object representation
/**
 * View span as Span<const uint8_t>.
 */
template <class ElementType, size_t Extent>
Span<const uint8_t,
     span_details::calculate_byte_size<ElementType, Extent>::value>
AsBytes(Span<ElementType, Extent> s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size_bytes()};
}

/**
 * View span as Span<uint8_t>.
 */
template <class ElementType, size_t Extent,
          class = std::enable_if_t<!std::is_const_v<ElementType>>>
Span<uint8_t, span_details::calculate_byte_size<ElementType, Extent>::value>
AsWritableBytes(Span<ElementType, Extent> s) {
  return {reinterpret_cast<uint8_t*>(s.data()), s.size_bytes()};
}

/**
 * View a span of uint8_t as a span of char.
 */
inline Span<const char> AsChars(Span<const uint8_t> s) {
  return {reinterpret_cast<const char*>(s.data()), s.size()};
}

/**
 * View a writable span of uint8_t as a span of char.
 */
inline Span<char> AsWritableChars(Span<uint8_t> s) {
  return {reinterpret_cast<char*>(s.data()), s.size()};
}

/**
 * Create span from a zero-terminated C string. nullptr is
 * treated as the empty string.
 */
constexpr Span<const char> MakeStringSpan(const char* aZeroTerminated) {
  if (!aZeroTerminated) {
    return Span<const char>();
  }
  return Span<const char>(aZeroTerminated,
                          std::char_traits<char>::length(aZeroTerminated));
}

/**
 * Create span from a zero-terminated UTF-16 C string. nullptr is
 * treated as the empty string.
 */
constexpr Span<const char16_t> MakeStringSpan(const char16_t* aZeroTerminated) {
  if (!aZeroTerminated) {
    return Span<const char16_t>();
  }
  return Span<const char16_t>(
      aZeroTerminated, std::char_traits<char16_t>::length(aZeroTerminated));
}

}  // namespace mozilla

#endif  // mozilla_Span_h
