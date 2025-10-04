// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/common/base64.h"

#if defined(HAVE_GSL)
#  include <gsl/gsl>
#else
#  include <assert.h>
#endif
#include <cstring>
#include <limits>

#if defined(HAVE_ABSEIL)
#  include "absl/strings/escaping.h"
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
#if !defined(HAVE_ABSEIL)
namespace
{
using Base64EscapeChars   = const unsigned char[64];
using Base64UnescapeChars = const unsigned char[128];

static constexpr Base64EscapeChars kBase64EscapeCharsBasic = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

static constexpr Base64UnescapeChars kBase64UnescapeCharsBasic = {
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 62,  127, 127, 127, 63,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  127, 127, 127, 127, 127, 127, 127, 0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,
    11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  127, 127, 127, 127,
    127, 127, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  46,  47,  48,  49,  50,  51,  127, 127, 127, 127, 127};

static int Base64EscapeInternal(unsigned char *dest,
                                std::size_t dlen,
                                std::size_t *olen,
                                const unsigned char *src,
                                std::size_t slen,
                                Base64EscapeChars &base64_enc_map,
                                unsigned char padding_char)
{
  std::size_t i, n, nopadding;
  int C1, C2, C3;
  unsigned char *p;

  if (slen == 0)
  {
    *olen = 0;
    return 0;
  }

  n = (slen + 2) / 3;

  if (n > (std::numeric_limits<std::size_t>::max() - 1) / 4)
  {
    *olen = std::numeric_limits<std::size_t>::max();
    return -1;
  }

  n *= 4;

  // no padding
  if (0 == padding_char)
  {
    nopadding = slen % 3;
    if (0 != nopadding)
    {
      n -= 3 - nopadding;
    }
  }

  if ((dlen < n + 1) || (nullptr == dest))
  {
    *olen = n + 1;
    return -1;
  }

  n = (slen / 3) * 3;

  for (i = 0, p = dest; i < n; i += 3)
  {
    C1 = *src++;
    C2 = *src++;
    C3 = *src++;

    *p++ = base64_enc_map[(C1 >> 2) & 0x3F];
    *p++ = base64_enc_map[(((C1 & 3) << 4) + (C2 >> 4)) & 0x3F];
    *p++ = base64_enc_map[(((C2 & 15) << 2) + (C3 >> 6)) & 0x3F];
    *p++ = base64_enc_map[C3 & 0x3F];
  }

  if (i < slen)
  {
    C1 = *src++;
    C2 = ((i + 1) < slen) ? *src++ : 0;

    *p++ = base64_enc_map[(C1 >> 2) & 0x3F];
    *p++ = base64_enc_map[(((C1 & 3) << 4) + (C2 >> 4)) & 0x3F];

    if ((i + 1) < slen)
    {
      *p++ = base64_enc_map[((C2 & 15) << 2) & 0x3F];
    }
    else if (padding_char)
    {
      *p++ = padding_char;
    }

    if (padding_char)
    {
      *p++ = padding_char;
    }
  }

  *olen = static_cast<std::size_t>(p - dest);
  *p    = 0;

  return 0;
}

static inline int Base64EscapeInternal(std::string &dest,
                                       const unsigned char *src,
                                       std::size_t slen,
                                       Base64EscapeChars &base64_enc_map,
                                       unsigned char padding_char)
{
  std::size_t olen = 0;
  Base64EscapeInternal(nullptr, 0, &olen, src, slen, base64_enc_map, padding_char);
  dest.resize(olen);

  if (nullptr == src || 0 == slen)
  {
    return 0;
  }

  int ret = Base64EscapeInternal(reinterpret_cast<unsigned char *>(&dest[0]), dest.size(), &olen,
                                 src, slen, base64_enc_map, padding_char);
#  if defined(HAVE_GSL)
  Expects(0 != ret || dest.size() == olen + 1);
#  else
  assert(0 != ret || dest.size() == olen + 1);
#  endif
  // pop back last zero
  if (!dest.empty() && *dest.rbegin() == 0)
  {
    dest.resize(dest.size() - 1);
  }
  return ret;
}

static int Base64UnescapeInternal(unsigned char *dst,
                                  std::size_t dlen,
                                  std::size_t *olen,
                                  const unsigned char *src,
                                  std::size_t slen,
                                  Base64UnescapeChars &base64_dec_map,
                                  unsigned char padding_char)
{
  std::size_t i, n;
  std::size_t j, x;
  std::size_t valid_slen, line_len;
  unsigned char *p;

  /* First pass: check for validity and get output length */
  for (i = n = j = valid_slen = line_len = 0; i < slen; i++)
  {
    /* Skip spaces before checking for EOL */
    x = 0;
    while (i < slen && (src[i] == ' ' || src[i] == '\t'))
    {
      ++i;
      ++x;
    }

    /* Spaces at end of buffer are OK */
    if (i == slen)
      break;

    if (src[i] == '\r' || src[i] == '\n')
    {
      line_len = 0;
      continue;
    }

    /* Space inside a line is an error */
    if (x != 0 && line_len != 0)
      return -2;

    ++valid_slen;
    ++line_len;
    if (src[i] == padding_char)
    {
      if (++j > 2 || (valid_slen & 3) == 1 || (valid_slen & 3) == 2)
      {
        return -2;
      }
    }
    else
    {
      if (src[i] > 127 || base64_dec_map[src[i]] == 127)
        return -2;
    }

    if (base64_dec_map[src[i]] < 64 && j != 0)
      return -2;

    n++;
  }

  if (n == 0)
  {
    *olen = 0;
    return 0;
  }

  // no padding, add j to padding length
  if (valid_slen & 3)
  {
    j += 4 - (valid_slen & 3);
    n += 4 - (valid_slen & 3);
  }

  /* The following expression is to calculate the following formula without
   * risk of integer overflow in n:
   *     n = ( ( n * 6 ) + 7 ) >> 3;
   */
  n = (6 * (n >> 3)) + ((6 * (n & 0x7) + 7) >> 3);
  n -= j;

  if (dst == nullptr || dlen < n)
  {
    *olen = n;
    return -1;
  }

  for (n = x = 0, p = dst; i > 0; i--, src++)
  {
    if (*src == '\r' || *src == '\n' || *src == ' ' || *src == '\t')
      continue;
    if (*src == padding_char)
      continue;

    x = (x << 6) | (base64_dec_map[*src] & 0x3F);

    if (++n == 4)
    {
      n    = 0;
      *p++ = static_cast<unsigned char>(x >> 16);
      *p++ = static_cast<unsigned char>(x >> 8);
      *p++ = static_cast<unsigned char>(x);
    }
  }

  // no padding, the tail code
  if (n == 2)
  {
    *p++ = static_cast<unsigned char>(x >> 4);
  }
  else if (n == 3)
  {
    *p++ = static_cast<unsigned char>(x >> 10);
    *p++ = static_cast<unsigned char>(x >> 2);
  }

  *olen = static_cast<std::size_t>(p - dst);

  return 0;
}

}  // namespace
#endif

// Base64Escape()
//
// Encodes a `src` string into a base64-encoded 'dest' string with padding
// characters. This function conforms with RFC 4648 section 4 (base64) and RFC
// 2045.
OPENTELEMETRY_EXPORT void Base64Escape(opentelemetry::nostd::string_view src, std::string *dest)
{
  if (nullptr == dest || src.empty())
  {
    return;
  }

#if defined(HAVE_ABSEIL)
  absl::Base64Escape(absl::string_view{src.data(), src.size()}, dest);
#else
  Base64EscapeInternal(*dest, reinterpret_cast<const unsigned char *>(src.data()), src.size(),
                       kBase64EscapeCharsBasic, '=');
#endif
}

OPENTELEMETRY_EXPORT std::string Base64Escape(opentelemetry::nostd::string_view src)
{
  std::string result;

  Base64Escape(src, &result);
  return result;
}

OPENTELEMETRY_EXPORT bool Base64Unescape(opentelemetry::nostd::string_view src, std::string *dest)
{
  if (nullptr == dest)
  {
    return false;
  }

#if defined(HAVE_ABSEIL)
  return absl::Base64Unescape(absl::string_view{src.data(), src.size()}, dest);
#else
  if (src.empty())
  {
    return true;
  }

  std::size_t olen = 0;

  if (-2 == Base64UnescapeInternal(nullptr, 0, &olen,
                                   reinterpret_cast<const unsigned char *>(src.data()), src.size(),
                                   kBase64UnescapeCharsBasic, '='))
  {
    return false;
  }

  dest->resize(olen);
  Base64UnescapeInternal(reinterpret_cast<unsigned char *>(&(*dest)[0]), dest->size(), &olen,
                         reinterpret_cast<const unsigned char *>(src.data()), src.size(),
                         kBase64UnescapeCharsBasic, '=');
  return true;
#endif
}

}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
