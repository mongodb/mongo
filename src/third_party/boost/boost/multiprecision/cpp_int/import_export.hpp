///////////////////////////////////////////////////////////////
//  Copyright 2015 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_CPP_INT_IMPORT_EXPORT_HPP
#define BOOST_MP_CPP_INT_IMPORT_EXPORT_HPP

#include <climits>
#include <cstring>
#include <boost/multiprecision/detail/endian.hpp>

namespace boost {
namespace multiprecision {

namespace detail {

template <class Backend, class Unsigned>
void assign_bits(Backend& val, Unsigned bits, std::size_t bit_location, std::size_t chunk_bits, const std::integral_constant<bool, false>& tag)
{
   std::size_t limb  = bit_location / (sizeof(limb_type) * CHAR_BIT);
   std::size_t shift = bit_location % (sizeof(limb_type) * CHAR_BIT);

   limb_type mask = chunk_bits >= sizeof(limb_type) * CHAR_BIT ? ~static_cast<limb_type>(0u) : (static_cast<limb_type>(1u) << chunk_bits) - 1;

   limb_type value = static_cast<limb_type>(bits & mask) << shift;
   if (value)
   {
      if (val.size() == limb)
      {
         val.resize(limb + 1, limb + 1);
         if (val.size() > limb)
            val.limbs()[limb] = value;
      }
      else if (val.size() > limb)
         val.limbs()[limb] |= value;
   }
   if (chunk_bits > sizeof(limb_type) * CHAR_BIT - shift)
   {
      shift = sizeof(limb_type) * CHAR_BIT - shift;
      chunk_bits -= shift;
      bit_location += shift;
      bits >>= shift;
      if (bits)
         assign_bits(val, bits, bit_location, chunk_bits, tag);
   }
}
template <class Backend, class Unsigned>
void assign_bits(Backend& val, Unsigned bits, std::size_t bit_location, std::size_t chunk_bits, const std::integral_constant<bool, true>&)
{
   using local_limb_type = typename Backend::local_limb_type;
   //
   // Check for possible overflow, this may trigger an exception, or have no effect
   // depending on whether this is a checked integer or not:
   //
   if ((bit_location >= sizeof(local_limb_type) * CHAR_BIT) && bits)
      val.resize(2, 2);
   else
   {
      local_limb_type mask  = chunk_bits >= sizeof(local_limb_type) * CHAR_BIT ? ~static_cast<local_limb_type>(0u) : (static_cast<local_limb_type>(1u) << chunk_bits) - 1;
      local_limb_type value = (static_cast<local_limb_type>(bits) & mask) << bit_location;
      *val.limbs() |= value;
      //
      // Check for overflow bits:
      //
      bit_location = sizeof(local_limb_type) * CHAR_BIT - bit_location;
      if ((bit_location < sizeof(bits) * CHAR_BIT) && (bits >>= bit_location))
         val.resize(2, 2); // May throw!
   }
}

template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked, class Allocator>
inline void resize_to_bit_size(cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>& newval, std::size_t bits, const std::integral_constant<bool, false>&)
{
   std::size_t limb_count = static_cast<unsigned>(bits / (sizeof(limb_type) * CHAR_BIT));
   if (bits % (sizeof(limb_type) * CHAR_BIT))
      ++limb_count;
   constexpr const std::size_t max_limbs = MaxBits ? MaxBits / (CHAR_BIT * sizeof(limb_type)) + ((MaxBits % (CHAR_BIT * sizeof(limb_type))) ? 1 : 0) : (std::numeric_limits<unsigned>::max)();
   if (limb_count > max_limbs)
      limb_count = max_limbs;
   newval.resize(limb_count, limb_count);
   std::memset(newval.limbs(), 0, newval.size() * sizeof(limb_type));
}
template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked, class Allocator>
inline void resize_to_bit_size(cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>& newval, unsigned, const std::integral_constant<bool, true>&)
{
   *newval.limbs() = 0;
}

template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked, class Allocator, expression_template_option ExpressionTemplates, class Iterator>
number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>&
import_bits_generic(
    number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>& val, Iterator i, Iterator j, std::size_t chunk_size = 0, bool msv_first = true)
{
   typename number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>::backend_type newval;

   using value_type = typename std::iterator_traits<Iterator>::value_type                                  ;
   using unsigned_value_type = typename boost::multiprecision::detail::make_unsigned<value_type>::type                                        ;
   using difference_type = typename std::iterator_traits<Iterator>::difference_type                             ;
   using size_type = typename boost::multiprecision::detail::make_unsigned<difference_type>::type                                   ;
   using tag_type = typename cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>::trivial_tag;

   if (!chunk_size)
      chunk_size = std::numeric_limits<value_type>::digits;

   size_type limbs = std::distance(i, j);
   size_type bits  = limbs * chunk_size;

   detail::resize_to_bit_size(newval, static_cast<unsigned>(bits), tag_type());

   difference_type bit_location        = msv_first ? bits - chunk_size : 0;
   difference_type bit_location_change = msv_first ? -static_cast<difference_type>(chunk_size) : chunk_size;

   while (i != j)
   {
      detail::assign_bits(newval, static_cast<unsigned_value_type>(*i), static_cast<std::size_t>(bit_location), chunk_size, tag_type());
      ++i;
      bit_location += bit_location_change;
   }

   newval.normalize();

   val.backend().swap(newval);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked, class Allocator, expression_template_option ExpressionTemplates, class T>
inline typename std::enable_if< !boost::multiprecision::backends::is_trivial_cpp_int<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator> >::value, number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>&>::type
import_bits_fast(
    number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>& val, T* i, T* j, std::size_t chunk_size = 0)
{
   std::size_t byte_len = (j - i) * (chunk_size ? chunk_size / CHAR_BIT : sizeof(*i));
   std::size_t limb_len = byte_len / sizeof(limb_type);
   if (byte_len % sizeof(limb_type))
      ++limb_len;
   cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>& result = val.backend();
   result.resize(static_cast<unsigned>(limb_len), static_cast<unsigned>(limb_len)); // checked types may throw here if they're not large enough to hold the data!
   result.limbs()[result.size() - 1] = 0u;
   std::memcpy(result.limbs(), i, (std::min)(byte_len, result.size() * sizeof(limb_type)));
   result.normalize(); // In case data has leading zeros.
   return val;
}
template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked, class Allocator, expression_template_option ExpressionTemplates, class T>
inline typename std::enable_if<boost::multiprecision::backends::is_trivial_cpp_int<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator> >::value, number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>&>::type
import_bits_fast(
    number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>& val, T* i, T* j, std::size_t chunk_size = 0)
{
   cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>& result   = val.backend();
   std::size_t                                                      byte_len = (j - i) * (chunk_size ? chunk_size / CHAR_BIT : sizeof(*i));
   std::size_t                                                      limb_len = byte_len / sizeof(result.limbs()[0]);
   if (byte_len % sizeof(result.limbs()[0]))
      ++limb_len;
   result.limbs()[0] = 0u;
   result.resize(static_cast<unsigned>(limb_len), static_cast<unsigned>(limb_len)); // checked types may throw here if they're not large enough to hold the data!
   std::memcpy(result.limbs(), i, (std::min)(byte_len, result.size() * sizeof(result.limbs()[0])));
   result.normalize(); // In case data has leading zeros.
   return val;
}
} // namespace detail

template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked, class Allocator, expression_template_option ExpressionTemplates, class Iterator>
inline number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>&
import_bits(
    number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>& val, Iterator i, Iterator j, std::size_t chunk_size = 0, bool msv_first = true)
{
   return detail::import_bits_generic(val, i, j, chunk_size, msv_first);
}

template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked, class Allocator, expression_template_option ExpressionTemplates, class T>
inline number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>&
import_bits(
    number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>& val, T* i, T* j, std::size_t chunk_size = 0, bool msv_first = true)
{
#if BOOST_MP_ENDIAN_LITTLE_BYTE
   if (((chunk_size % CHAR_BIT) == 0) && !msv_first)
      return detail::import_bits_fast(val, i, j, chunk_size);
#endif
   return detail::import_bits_generic(val, i, j, chunk_size, msv_first);
}

namespace detail {

template <class Backend>
std::uintmax_t extract_bits(const Backend& val, std::size_t location, std::size_t count, const std::integral_constant<bool, false>& tag)
{
   std::size_t         limb   = location / (sizeof(limb_type) * CHAR_BIT);
   std::size_t         shift  = location % (sizeof(limb_type) * CHAR_BIT);
   std::uintmax_t result = 0;
   std::uintmax_t mask   = count == std::numeric_limits<std::uintmax_t>::digits ? ~static_cast<std::uintmax_t>(0) : (static_cast<std::uintmax_t>(1u) << count) - 1;
   if (count > (sizeof(limb_type) * CHAR_BIT - shift))
   {
      result = extract_bits(val, location + sizeof(limb_type) * CHAR_BIT - shift, count - sizeof(limb_type) * CHAR_BIT + shift, tag);
      result <<= sizeof(limb_type) * CHAR_BIT - shift;
   }
   if (limb < val.size())
      result |= (val.limbs()[limb] >> shift) & mask;
   return result;
}

template <class Backend>
inline std::uintmax_t extract_bits(const Backend& val, std::size_t location, std::size_t count, const std::integral_constant<bool, true>&)
{
   typename Backend::local_limb_type result = *val.limbs();
   typename Backend::local_limb_type mask   = count >= std::numeric_limits<typename Backend::local_limb_type>::digits ? ~static_cast<typename Backend::local_limb_type>(0) : (static_cast<typename Backend::local_limb_type>(1u) << count) - 1;
   return (result >> location) & mask;
}

} // namespace detail

template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked, class Allocator, expression_template_option ExpressionTemplates, class OutputIterator>
OutputIterator export_bits(
    const number<cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>& val, OutputIterator out, std::size_t chunk_size, bool msv_first = true)
{
#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
   using tag_type = typename cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>::trivial_tag;
   if (!val)
   {
      *out = 0;
      ++out;
      return out;
   }
   std::size_t bitcount = boost::multiprecision::backends::eval_msb_imp(val.backend()) + 1;
   std::size_t chunks   = bitcount / chunk_size;
   if (bitcount % chunk_size)
      ++chunks;

   std::ptrdiff_t bit_location = msv_first ? bitcount - chunk_size : 0;
   std::ptrdiff_t bit_step     = msv_first ? -static_cast<int>(chunk_size) : chunk_size;
   while (bit_location % bit_step)
      ++bit_location;

   do
   {
      *out = detail::extract_bits(val.backend(), bit_location, chunk_size, tag_type());
      ++out;
      bit_location += bit_step;
   } while ((bit_location >= 0) && (bit_location < static_cast<int>(bitcount)));

   return out;
#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
}

}
} // namespace boost::multiprecision

#endif // BOOST_MP_CPP_INT_IMPORT_EXPORT_HPP
