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

// Adapted from https://github.com/Microsoft/GSL/blob/3819df6e378ffccf0e29465afe99c3b324c2aa70/include/gsl/span
// and https://github.com/Microsoft/GSL/blob/3819df6e378ffccf0e29465afe99c3b324c2aa70/include/gsl/gsl_util

#ifndef mozilla_Span_h
#define mozilla_Span_h

#include "mozilla/Array.h"
#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/Move.h"
#include "mozilla/TypeTraits.h"
#include "mozilla/UniquePtr.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

// Classifications for reasons why constexpr was removed in C++14 to C++11
// conversion. Once we upgrade compilers, we can try defining each of these
// to constexpr to restore a category of constexprs at a time.
#if !defined(__clang__) && defined(__GNUC__) && __cpp_constexpr < 201304
#define MOZ_SPAN_ASSERTION_CONSTEXPR
#define MOZ_SPAN_GCC_CONSTEXPR
#define MOZ_SPAN_EXPLICITLY_DEFAULTED_CONSTEXPR
#define MOZ_SPAN_CONSTEXPR_NOT_JUST_RETURN
#define MOZ_SPAN_NON_CONST_CONSTEXPR
#else
#define MOZ_SPAN_ASSERTION_CONSTEXPR constexpr
#define MOZ_SPAN_GCC_CONSTEXPR constexpr
#define MOZ_SPAN_EXPLICITLY_DEFAULTED_CONSTEXPR constexpr
#define MOZ_SPAN_CONSTEXPR_NOT_JUST_RETURN constexpr
#define MOZ_SPAN_NON_CONST_CONSTEXPR constexpr
#endif

#ifdef _MSC_VER
#pragma warning(push)

// turn off some warnings that are noisy about our MOZ_RELEASE_ASSERT statements
#pragma warning(disable : 4127) // conditional expression is constant

// blanket turn off warnings from CppCoreCheck for now
// so people aren't annoyed by them when running the tool.
// more targeted suppressions will be added in a future update to the GSL
#pragma warning(disable : 26481 26482 26483 26485 26490 26491 26492 26493 26495)

#if _MSC_VER < 1910
#pragma push_macro("constexpr")
#define constexpr /*constexpr*/

#endif            // _MSC_VER < 1910
#endif            // _MSC_VER

namespace mozilla {

// Stuff from gsl_util

// narrow_cast(): a searchable way to do narrowing casts of values
template<class T, class U>
inline constexpr T
narrow_cast(U&& u)
{
  return static_cast<T>(mozilla::Forward<U>(u));
}

// end gsl_util

// [views.constants], constants
// This was -1 in gsl::span, but using size_t for sizes instead of ptrdiff_t
// and reserving a magic value that realistically doesn't occur in
// compile-time-constant Span sizes makes things a lot less messy in terms of
// comparison between signed and unsigned.
constexpr const size_t dynamic_extent = mozilla::MaxValue<size_t>::value;

template<class ElementType, size_t Extent = dynamic_extent>
class Span;

// implementation details
namespace span_details {

inline size_t strlen16(const char16_t* aZeroTerminated) {
  size_t len = 0;
  while (*(aZeroTerminated++)) {
    len++;
  }
  return len;
}

// C++14 types that we don't have because we build as C++11.
template<class T>
using remove_cv_t = typename mozilla::RemoveCV<T>::Type;
template<class T>
using remove_const_t = typename mozilla::RemoveConst<T>::Type;
template<bool B, class T, class F>
using conditional_t = typename mozilla::Conditional<B, T, F>::Type;
template<class T>
using add_pointer_t = typename mozilla::AddPointer<T>::Type;
template<bool B, class T = void>
using enable_if_t = typename mozilla::EnableIf<B, T>::Type;

template<class T>
struct is_span_oracle : mozilla::FalseType
{
};

template<class ElementType, size_t Extent>
struct is_span_oracle<mozilla::Span<ElementType, Extent>> : mozilla::TrueType
{
};

template<class T>
struct is_span : public is_span_oracle<remove_cv_t<T>>
{
};

template<class T>
struct is_std_array_oracle : mozilla::FalseType
{
};

template<class ElementType, size_t Extent>
struct is_std_array_oracle<std::array<ElementType, Extent>> : mozilla::TrueType
{
};

template<class T>
struct is_std_array : public is_std_array_oracle<remove_cv_t<T>>
{
};

template<size_t From, size_t To>
struct is_allowed_extent_conversion
  : public mozilla::IntegralConstant<bool,
                                  From == To ||
                                    From == mozilla::dynamic_extent ||
                                    To == mozilla::dynamic_extent>
{
};

template<class From, class To>
struct is_allowed_element_type_conversion
  : public mozilla::IntegralConstant<bool, mozilla::IsConvertible<From (*)[], To (*)[]>::value>
{
};

template<class Span, bool IsConst>
class span_iterator
{
  using element_type_ = typename Span::element_type;

public:
  using iterator_category = std::random_access_iterator_tag;
  using value_type = remove_const_t<element_type_>;
  using difference_type = typename Span::index_type;

  using reference = conditional_t<IsConst, const element_type_, element_type_>&;
  using pointer = add_pointer_t<reference>;

  constexpr span_iterator() : span_iterator(nullptr, 0) {}

  MOZ_SPAN_ASSERTION_CONSTEXPR span_iterator(const Span* span,
                                             typename Span::index_type index)
    : span_(span)
    , index_(index)
  {
    MOZ_RELEASE_ASSERT(span == nullptr ||
                       (index_ >= 0 && index <= span_->Length()));
  }

  friend class span_iterator<Span, true>;
  constexpr MOZ_IMPLICIT span_iterator(const span_iterator<Span, false>& other)
    : span_iterator(other.span_, other.index_)
  {
  }

  MOZ_SPAN_EXPLICITLY_DEFAULTED_CONSTEXPR span_iterator<Span, IsConst>&
  operator=(const span_iterator<Span, IsConst>&) = default;

  MOZ_SPAN_GCC_CONSTEXPR reference operator*() const
  {
    MOZ_RELEASE_ASSERT(span_);
    return (*span_)[index_];
  }

  constexpr pointer operator->() const
  {
    MOZ_RELEASE_ASSERT(span_);
    return &((*span_)[index_]);
  }

  MOZ_SPAN_NON_CONST_CONSTEXPR span_iterator& operator++()
  {
    MOZ_RELEASE_ASSERT(span_ && index_ >= 0 && index_ < span_->Length());
    ++index_;
    return *this;
  }

  constexpr span_iterator operator++(int)
  {
    auto ret = *this;
    ++(*this);
    return ret;
  }

  MOZ_SPAN_NON_CONST_CONSTEXPR span_iterator& operator--()
  {
    MOZ_RELEASE_ASSERT(span_ && index_ > 0 && index_ <= span_->Length());
    --index_;
    return *this;
  }

  constexpr span_iterator operator--(int)
  {
    auto ret = *this;
    --(*this);
    return ret;
  }

  MOZ_SPAN_CONSTEXPR_NOT_JUST_RETURN span_iterator
  operator+(difference_type n) const
  {
    auto ret = *this;
    return ret += n;
  }

  MOZ_SPAN_GCC_CONSTEXPR span_iterator& operator+=(difference_type n)
  {
    MOZ_RELEASE_ASSERT(span_ && (index_ + n) >= 0 &&
                       (index_ + n) <= span_->Length());
    index_ += n;
    return *this;
  }

  constexpr span_iterator
  operator-(difference_type n) const
  {
    auto ret = *this;
    return ret -= n;
  }

  constexpr span_iterator& operator-=(difference_type n)

  {
    return *this += -n;
  }

  MOZ_SPAN_GCC_CONSTEXPR difference_type
  operator-(const span_iterator& rhs) const
  {
    MOZ_RELEASE_ASSERT(span_ == rhs.span_);
    return index_ - rhs.index_;
  }

  constexpr reference operator[](difference_type n) const
  {
    return *(*this + n);
  }

  constexpr friend bool operator==(const span_iterator& lhs,
                                   const span_iterator& rhs)
  {
    return lhs.span_ == rhs.span_ && lhs.index_ == rhs.index_;
  }

  constexpr friend bool operator!=(const span_iterator& lhs,
                                   const span_iterator& rhs)
  {
    return !(lhs == rhs);
  }

  MOZ_SPAN_GCC_CONSTEXPR friend bool operator<(const span_iterator& lhs,
                                               const span_iterator& rhs)
  {
    MOZ_RELEASE_ASSERT(lhs.span_ == rhs.span_);
    return lhs.index_ < rhs.index_;
  }

  constexpr friend bool operator<=(const span_iterator& lhs,
                                                const span_iterator& rhs)
  {
    return !(rhs < lhs);
  }

  constexpr friend bool operator>(const span_iterator& lhs,
                                               const span_iterator& rhs)
  {
    return rhs < lhs;
  }

  constexpr friend bool operator>=(const span_iterator& lhs,
                                                const span_iterator& rhs)
  {
    return !(rhs > lhs);
  }

  void swap(span_iterator& rhs)
  {
    std::swap(index_, rhs.index_);
    std::swap(span_, rhs.span_);
  }

protected:
  const Span* span_;
  size_t index_;
};

template<class Span, bool IsConst>
inline constexpr span_iterator<Span, IsConst>
operator+(typename span_iterator<Span, IsConst>::difference_type n,
          const span_iterator<Span, IsConst>& rhs)
{
  return rhs + n;
}

template<size_t Ext>
class extent_type
{
public:
  using index_type = size_t;

  static_assert(Ext >= 0, "A fixed-size Span must be >= 0 in size.");

  constexpr extent_type() {}

  template<index_type Other>
  MOZ_SPAN_ASSERTION_CONSTEXPR MOZ_IMPLICIT extent_type(extent_type<Other> ext)
  {
    static_assert(
      Other == Ext || Other == dynamic_extent,
      "Mismatch between fixed-size extent and size of initializing data.");
    MOZ_RELEASE_ASSERT(ext.size() == Ext);
  }

  MOZ_SPAN_ASSERTION_CONSTEXPR MOZ_IMPLICIT extent_type(index_type length)
  {
    MOZ_RELEASE_ASSERT(length == Ext);
  }

  constexpr index_type size() const { return Ext; }
};

template<>
class extent_type<dynamic_extent>
{
public:
  using index_type = size_t;

  template<index_type Other>
  explicit constexpr extent_type(extent_type<Other> ext)
    : size_(ext.size())
  {
  }

  explicit constexpr extent_type(index_type length)
    : size_(length)
  {
  }

  constexpr index_type size() const { return size_; }

private:
  index_type size_;
};
} // namespace span_details

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
 * Spans. MakeSpan() functions can be used for explicit conversion in other
 * contexts. (Span itself autoconverts into mozilla::Range.)
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
 * In addition to having constructors and MakeSpan() functions that take
 * various well-known types, a Span for an arbitrary type can be constructed
 * (via constructor or MakeSpan()) from a pointer and a length or a pointer
 * and another pointer pointing just past the last element.
 *
 * A Span<const char> or Span<const char16_t> can be obtained for const char*
 * or const char16_t pointing to a zero-terminated string using the
 * MakeStringSpan() function. Corresponding implicit constructor does not exist
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
 */
template<class ElementType, size_t Extent>
class Span
{
public:
  // constants and types
  using element_type = ElementType;
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
  // "Dependent" is needed to make "span_details::enable_if_t<(Dependent || Extent == 0 || Extent == mozilla::MaxValue<size_t>::value)>" SFINAE,
  // since "span_details::enable_if_t<(Extent == 0 || Extent == mozilla::MaxValue<size_t>::value)>" is ill-formed when Extent is neither of the extreme values.
  /**
   * Constructor with no args.
   */
  template<
    bool Dependent = false,
    class = span_details::enable_if_t<
      (Dependent || Extent == 0 || Extent == mozilla::MaxValue<size_t>::value)>>
  constexpr Span()
    : storage_(nullptr, span_details::extent_type<0>())
  {
  }

  /**
   * Constructor for nullptr.
   */
  constexpr MOZ_IMPLICIT Span(std::nullptr_t) : Span() {}

  /**
   * Constructor for pointer and length.
   */
  constexpr Span(pointer aPtr, index_type aLength)
    : storage_(aPtr, aLength)
  {
  }

  /**
   * Constructor for start pointer and pointer past end.
   */
  constexpr Span(pointer aStartPtr, pointer aEndPtr)
    : storage_(aStartPtr, std::distance(aStartPtr, aEndPtr))
  {
  }

  /**
   * Constructor for C array.
   */
  template<size_t N>
  constexpr MOZ_IMPLICIT Span(element_type (&aArr)[N])
    : storage_(&aArr[0], span_details::extent_type<N>())
  {
  }

  // Implicit constructors for char* and char16_t* pointers are deleted in order
  // to avoid accidental construction in cases where a pointer does not point to
  // a zero-terminated string. A Span<const char> or Span<const char16_t> can be
  // obtained for const char* or const char16_t pointing to a zero-terminated
  // string using the MakeStringSpan() function.
  Span(char* aStr) = delete;
  Span(const char* aStr) = delete;
  Span(char16_t* aStr) = delete;
  Span(const char16_t* aStr) = delete;

  /**
   * Constructor for std::array.
   */
  template<size_t N,
           class ArrayElementType = span_details::remove_const_t<element_type>>
  constexpr MOZ_IMPLICIT Span(std::array<ArrayElementType, N>& aArr)
    : storage_(&aArr[0], span_details::extent_type<N>())
  {
  }

  /**
   * Constructor for const std::array.
   */
  template<size_t N>
  constexpr MOZ_IMPLICIT Span(
    const std::array<span_details::remove_const_t<element_type>, N>& aArr)
    : storage_(&aArr[0], span_details::extent_type<N>())
  {
  }

  /**
   * Constructor for mozilla::Array.
   */
  template<size_t N,
           class ArrayElementType = span_details::remove_const_t<element_type>>
  constexpr MOZ_IMPLICIT Span(mozilla::Array<ArrayElementType, N>& aArr)
    : storage_(&aArr[0], span_details::extent_type<N>())
  {
  }

  /**
   * Constructor for const mozilla::Array.
   */
  template<size_t N>
  constexpr MOZ_IMPLICIT Span(
    const mozilla::Array<span_details::remove_const_t<element_type>, N>& aArr)
    : storage_(&aArr[0], span_details::extent_type<N>())
  {
  }

  /**
   * Constructor for mozilla::UniquePtr holding an array and length.
   */
  template<class ArrayElementType = std::add_pointer<element_type>>
  constexpr Span(const mozilla::UniquePtr<ArrayElementType>& aPtr,
                 index_type aLength)
    : storage_(aPtr.get(), aLength)
  {
  }

  // NB: the SFINAE here uses .data() as a incomplete/imperfect proxy for the requirement
  // on Container to be a contiguous sequence container.
  /**
   * Constructor for standard-library containers.
   */
  template<
    class Container,
    class = span_details::enable_if_t<
      !span_details::is_span<Container>::value &&
      !span_details::is_std_array<Container>::value &&
      mozilla::IsConvertible<typename Container::pointer, pointer>::value &&
      mozilla::IsConvertible<typename Container::pointer,
                          decltype(mozilla::DeclVal<Container>().data())>::value>>
  constexpr MOZ_IMPLICIT Span(Container& cont)
    : Span(cont.data(), ReleaseAssertedCast<index_type>(cont.size()))
  {
  }

  /**
   * Constructor for standard-library containers (const version).
   */
  template<
    class Container,
    class = span_details::enable_if_t<
      mozilla::IsConst<element_type>::value &&
      !span_details::is_span<Container>::value &&
      mozilla::IsConvertible<typename Container::pointer, pointer>::value &&
      mozilla::IsConvertible<typename Container::pointer,
                          decltype(mozilla::DeclVal<Container>().data())>::value>>
  constexpr MOZ_IMPLICIT Span(const Container& cont)
    : Span(cont.data(), ReleaseAssertedCast<index_type>(cont.size()))
  {
  }

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
  template<
    class OtherElementType,
    size_t OtherExtent,
    class = span_details::enable_if_t<
      span_details::is_allowed_extent_conversion<OtherExtent, Extent>::value &&
      span_details::is_allowed_element_type_conversion<OtherElementType,
                                                       element_type>::value>>
  constexpr MOZ_IMPLICIT Span(const Span<OtherElementType, OtherExtent>& other)
    : storage_(other.data(),
               span_details::extent_type<OtherExtent>(other.size()))
  {
  }

  /**
   * Constructor from other Span with conversion of element type.
   */
  template<
    class OtherElementType,
    size_t OtherExtent,
    class = span_details::enable_if_t<
      span_details::is_allowed_extent_conversion<OtherExtent, Extent>::value &&
      span_details::is_allowed_element_type_conversion<OtherElementType,
                                                       element_type>::value>>
  constexpr MOZ_IMPLICIT Span(Span<OtherElementType, OtherExtent>&& other)
    : storage_(other.data(),
               span_details::extent_type<OtherExtent>(other.size()))
  {
  }

  ~Span() = default;
  MOZ_SPAN_EXPLICITLY_DEFAULTED_CONSTEXPR Span& operator=(const Span& other)
    = default;

  MOZ_SPAN_EXPLICITLY_DEFAULTED_CONSTEXPR Span& operator=(Span&& other)
    = default;

  // [Span.sub], Span subviews
  /**
   * Subspan with first N elements with compile-time N.
   */
  template<size_t Count>
  constexpr Span<element_type, Count> First() const
  {
    MOZ_RELEASE_ASSERT(Count <= size());
    return { data(), Count };
  }

  /**
   * Subspan with last N elements with compile-time N.
   */
  template<size_t Count>
  constexpr Span<element_type, Count> Last() const
  {
    const size_t len = size();
    MOZ_RELEASE_ASSERT(Count <= len);
    return { data() + (len - Count), Count };
  }

  /**
   * Subspan with compile-time start index and length.
   */
  template<size_t Offset, size_t Count = dynamic_extent>
  constexpr Span<element_type, Count> Subspan() const
  {
    const size_t len = size();
    MOZ_RELEASE_ASSERT(Offset <= len &&
      (Count == dynamic_extent || (Offset + Count <= len)));
    return { data() + Offset,
             Count == dynamic_extent ? len - Offset : Count };
  }

  /**
   * Subspan with first N elements with run-time N.
   */
  constexpr Span<element_type, dynamic_extent> First(
    index_type aCount) const
  {
    MOZ_RELEASE_ASSERT(aCount <= size());
    return { data(), aCount };
  }

  /**
   * Subspan with last N elements with run-time N.
   */
  constexpr Span<element_type, dynamic_extent> Last(
    index_type aCount) const
  {
    const size_t len = size();
    MOZ_RELEASE_ASSERT(aCount <= len);
    return { data() + (len - aCount), aCount };
  }

  /**
   * Subspan with run-time start index and length.
   */
  constexpr Span<element_type, dynamic_extent> Subspan(
    index_type aStart,
    index_type aLength = dynamic_extent) const
  {
    const size_t len = size();
    MOZ_RELEASE_ASSERT(aStart <= len &&
                       (aLength == dynamic_extent ||
                        (aStart + aLength <= len)));
    return { data() + aStart,
             aLength == dynamic_extent ? len - aStart : aLength };
  }

  /**
   * Subspan with run-time start index. (Rust's &foo[start..])
   */
  constexpr Span<element_type, dynamic_extent> From(
    index_type aStart) const
  {
    return Subspan(aStart);
  }

  /**
   * Subspan with run-time exclusive end index. (Rust's &foo[..end])
   */
  constexpr Span<element_type, dynamic_extent> To(
    index_type aEnd) const
  {
    return Subspan(0, aEnd);
  }

  /**
   * Subspan with run-time start index and exclusive end index.
   * (Rust's &foo[start..end])
   */
  constexpr Span<element_type, dynamic_extent> FromTo(
    index_type aStart,
    index_type aEnd) const
  {
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
  constexpr index_type size_bytes() const
  {
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
  constexpr reference operator[](index_type idx) const
  {
    MOZ_RELEASE_ASSERT(idx < storage_.size());
    return data()[idx];
  }

  /**
   * Access element of span by index (standard-library duck typing version).
   */
  constexpr reference at(index_type idx) const { return this->operator[](idx); }

  constexpr reference operator()(index_type idx) const
  {
    return this->operator[](idx);
  }

  /**
   * Pointer to the first element of the span. The return value is never
   * nullptr, not ever for zero-length spans, so it can be passed as-is
   * to std::slice::from_raw_parts() in Rust.
   */
  constexpr pointer Elements() const { return data(); }

  /**
   * Pointer to the first element of the span (standard-libray duck typing version).
   * The return value is never nullptr, not ever for zero-length spans, so it can
   * be passed as-is to std::slice::from_raw_parts() in Rust.
   */
  constexpr pointer data() const { return storage_.data(); }

  // [Span.iter], Span iterator support
  iterator begin() const { return { this, 0 }; }
  iterator end() const { return { this, Length() }; }

  const_iterator cbegin() const { return { this, 0 }; }
  const_iterator cend() const { return { this, Length() }; }

  reverse_iterator rbegin() const
  {
    return reverse_iterator{ end() };
  }
  reverse_iterator rend() const
  {
    return reverse_iterator{ begin() };
  }

  const_reverse_iterator crbegin() const
  {
    return const_reverse_iterator{ cend() };
  }
  const_reverse_iterator crend() const
  {
    return const_reverse_iterator{ cbegin() };
  }

private:
  // this implementation detail class lets us take advantage of the
  // empty base class optimization to pay for only storage of a single
  // pointer in the case of fixed-size Spans
  template<class ExtentType>
  class storage_type : public ExtentType
  {
  public:
    template<class OtherExtentType>
    MOZ_SPAN_ASSERTION_CONSTEXPR storage_type(pointer elements,
                                              OtherExtentType ext)
      : ExtentType(ext)
      // Replace nullptr with 0x1 for Rust slice compatibility. See
      // https://doc.rust-lang.org/std/slice/fn.from_raw_parts.html
      , data_(elements ? elements : reinterpret_cast<pointer>(0x1))
    {
      const size_t extentSize = ExtentType::size();
      MOZ_RELEASE_ASSERT(
        (!elements && extentSize == 0) ||
        (elements && extentSize != mozilla::MaxValue<size_t>::value));
    }

    constexpr pointer data() const { return data_; }

  private:
    pointer data_;
  };

  storage_type<span_details::extent_type<Extent>> storage_;
};

// [Span.comparison], Span comparison operators
template<class ElementType, size_t FirstExtent, size_t SecondExtent>
inline constexpr bool
operator==(const Span<ElementType, FirstExtent>& l,
           const Span<ElementType, SecondExtent>& r)
{
  return (l.size() == r.size()) && std::equal(l.begin(), l.end(), r.begin());
}

template<class ElementType, size_t Extent>
inline constexpr bool
operator!=(const Span<ElementType, Extent>& l,
           const Span<ElementType, Extent>& r)
{
  return !(l == r);
}

template<class ElementType, size_t Extent>
inline constexpr bool
operator<(const Span<ElementType, Extent>& l,
          const Span<ElementType, Extent>& r)
{
  return std::lexicographical_compare(l.begin(), l.end(), r.begin(), r.end());
}

template<class ElementType, size_t Extent>
inline constexpr bool
operator<=(const Span<ElementType, Extent>& l,
           const Span<ElementType, Extent>& r)
{
  return !(l > r);
}

template<class ElementType, size_t Extent>
inline constexpr bool
operator>(const Span<ElementType, Extent>& l,
          const Span<ElementType, Extent>& r)
{
  return r < l;
}

template<class ElementType, size_t Extent>
inline constexpr bool
operator>=(const Span<ElementType, Extent>& l,
           const Span<ElementType, Extent>& r)
{
  return !(l < r);
}

namespace span_details {
// if we only supported compilers with good constexpr support then
// this pair of classes could collapse down to a constexpr function

// we should use a narrow_cast<> to go to size_t, but older compilers may not see it as
// constexpr
// and so will fail compilation of the template
template<class ElementType, size_t Extent>
struct calculate_byte_size
  : mozilla::IntegralConstant<size_t,
                           static_cast<size_t>(sizeof(ElementType) *
                                               static_cast<size_t>(Extent))>
{
};

template<class ElementType>
struct calculate_byte_size<ElementType, dynamic_extent>
  : mozilla::IntegralConstant<size_t, dynamic_extent>
{
};
}

// [Span.objectrep], views of object representation
/**
 * View span as Span<const uint8_t>.
 */
template<class ElementType, size_t Extent>
Span<const uint8_t,
     span_details::calculate_byte_size<ElementType, Extent>::value>
AsBytes(Span<ElementType, Extent> s)
{
  return { reinterpret_cast<const uint8_t*>(s.data()), s.size_bytes() };
}

/**
 * View span as Span<uint8_t>.
 */
template<class ElementType,
         size_t Extent,
         class = span_details::enable_if_t<!mozilla::IsConst<ElementType>::value>>
Span<uint8_t, span_details::calculate_byte_size<ElementType, Extent>::value>
AsWritableBytes(Span<ElementType, Extent> s)
{
  return { reinterpret_cast<uint8_t*>(s.data()), s.size_bytes() };
}

//
// MakeSpan() - Utility functions for creating Spans
//
/**
 * Create span from pointer and length.
 */
template<class ElementType>
Span<ElementType>
MakeSpan(ElementType* aPtr, typename Span<ElementType>::index_type aLength)
{
  return Span<ElementType>(aPtr, aLength);
}

/**
 * Create span from start pointer and pointer past end.
 */
template<class ElementType>
Span<ElementType>
MakeSpan(ElementType* aStartPtr, ElementType* aEndPtr)
{
  return Span<ElementType>(aStartPtr, aEndPtr);
}

/**
 * Create span from C array.
 * MakeSpan() does not permit creating Span objects from string literals (const
 * char or char16_t arrays) because the Span length would include the zero
 * terminator, which may surprise callers. Use MakeStringSpan() to create a
 * Span whose length that excludes the string literal's zero terminator or use
 * the MakeSpan() overload that accepts a pointer and length and specify the
 * string literal's full length.
 */
template<class ElementType, size_t N,
         class = span_details::enable_if_t<
                   !IsSame<ElementType, const char>::value &&
                   !IsSame<ElementType, const char16_t>::value>>
Span<ElementType> MakeSpan(ElementType (&aArr)[N])
{
  return Span<ElementType>(aArr, N);
}

/**
 * Create span from mozilla::Array.
 */
template<class ElementType, size_t N>
Span<ElementType>
MakeSpan(mozilla::Array<ElementType, N>& aArr)
{
  return aArr;
}

/**
 * Create span from const mozilla::Array.
 */
template<class ElementType, size_t N>
Span<const ElementType>
MakeSpan(const mozilla::Array<ElementType, N>& arr)
{
  return arr;
}

/**
 * Create span from standard-library container.
 */
template<class Container>
Span<typename Container::value_type>
MakeSpan(Container& cont)
{
  return Span<typename Container::value_type>(cont);
}

/**
 * Create span from standard-library container (const version).
 */
template<class Container>
Span<const typename Container::value_type>
MakeSpan(const Container& cont)
{
  return Span<const typename Container::value_type>(cont);
}

/**
 * Create span from smart pointer and length.
 */
template<class Ptr>
Span<typename Ptr::element_type>
MakeSpan(Ptr& aPtr, size_t aLength)
{
  return Span<typename Ptr::element_type>(aPtr, aLength);
}

/**
 * Create span from C string.
 */
inline Span<const char>
MakeStringSpan(const char* aZeroTerminated)
{
  return Span<const char>(aZeroTerminated, std::strlen(aZeroTerminated));
}

/**
 * Create span from UTF-16 C string.
 */
inline Span<const char16_t>
MakeStringSpan(const char16_t* aZeroTerminated)
{
  return Span<const char16_t>(aZeroTerminated, span_details::strlen16(aZeroTerminated));
}

} // namespace mozilla

#ifdef _MSC_VER
#if _MSC_VER < 1910
#undef constexpr
#pragma pop_macro("constexpr")

#endif // _MSC_VER < 1910

#pragma warning(pop)
#endif // _MSC_VER

#undef MOZ_SPAN_ASSERTION_CONSTEXPR
#undef MOZ_SPAN_GCC_CONSTEXPR
#undef MOZ_SPAN_EXPLICITLY_DEFAULTED_CONSTEXPR
#undef MOZ_SPAN_CONSTEXPR_NOT_JUST_RETURN
#undef MOZ_SPAN_NON_CONST_CONSTEXPR

#endif // mozilla_Span_h
