/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   dump.cpp
 * \author Andrey Semashev
 * \date   03.05.2013
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <ostream>
#include <boost/cstdint.hpp>
#include <boost/log/utility/manipulators/dump.hpp>
#if defined(_MSC_VER) && (defined(BOOST_LOG_USE_SSSE3) || defined(BOOST_LOG_USE_AVX2))
#include <boost/winapi/dll.hpp>
#include <intrin.h> // __cpuid
#endif
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

#if defined(BOOST_LOG_USE_SSSE3)
extern dump_data_char_t dump_data_char_ssse3;
extern dump_data_wchar_t dump_data_wchar_ssse3;
#if !defined(BOOST_NO_CXX11_CHAR16_T)
extern dump_data_char16_t dump_data_char16_ssse3;
#endif
#if !defined(BOOST_NO_CXX11_CHAR32_T)
extern dump_data_char32_t dump_data_char32_ssse3;
#endif
#endif
#if defined(BOOST_LOG_USE_AVX2)
extern dump_data_char_t dump_data_char_avx2;
extern dump_data_wchar_t dump_data_wchar_avx2;
#if !defined(BOOST_NO_CXX11_CHAR16_T)
extern dump_data_char16_t dump_data_char16_avx2;
#endif
#if !defined(BOOST_NO_CXX11_CHAR32_T)
extern dump_data_char32_t dump_data_char32_avx2;
#endif
#endif

enum { stride = 256 };

extern const char g_hex_char_table[2][16] =
{
    { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' },
    { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' }
};

template< typename CharT >
void dump_data_generic(const void* data, std::size_t size, std::basic_ostream< CharT >& strm)
{
    typedef CharT char_type;

    char_type buf[stride * 3u];

    const char* const char_table = g_hex_char_table[(strm.flags() & std::ios_base::uppercase) != 0];
    const std::size_t stride_count = size / stride, tail_size = size % stride;

    const uint8_t* p = static_cast< const uint8_t* >(data);
    char_type* buf_begin = buf + 1u; // skip the first space of the first chunk
    char_type* buf_end = buf + sizeof(buf) / sizeof(*buf);

    for (std::size_t i = 0; i < stride_count; ++i)
    {
        char_type* b = buf;
        for (unsigned int j = 0; j < stride; ++j, b += 3u, ++p)
        {
            uint32_t n = *p;
            b[0] = static_cast< char_type >(' ');
            b[1] = static_cast< char_type >(char_table[n >> 4]);
            b[2] = static_cast< char_type >(char_table[n & 0x0F]);
        }

        strm.write(buf_begin, buf_end - buf_begin);
        buf_begin = buf;
    }

    if (tail_size > 0)
    {
        char_type* b = buf;
        unsigned int i = 0;
        do
        {
            uint32_t n = *p;
            b[0] = static_cast< char_type >(' ');
            b[1] = static_cast< char_type >(char_table[n >> 4]);
            b[2] = static_cast< char_type >(char_table[n & 0x0F]);
            ++i;
            ++p;
            b += 3u;
        }
        while (i < tail_size);

        strm.write(buf_begin, b - buf_begin);
    }
}

BOOST_LOG_API dump_data_char_t* dump_data_char = &dump_data_generic< char >;
BOOST_LOG_API dump_data_wchar_t* dump_data_wchar = &dump_data_generic< wchar_t >;
#if !defined(BOOST_NO_CXX11_CHAR16_T)
BOOST_LOG_API dump_data_char16_t* dump_data_char16 = &dump_data_generic< char16_t >;
#endif
#if !defined(BOOST_NO_CXX11_CHAR32_T)
BOOST_LOG_API dump_data_char32_t* dump_data_char32 = &dump_data_generic< char32_t >;
#endif

#if defined(BOOST_LOG_USE_SSSE3) || defined(BOOST_LOG_USE_AVX2)

BOOST_LOG_ANONYMOUS_NAMESPACE {

struct function_pointer_initializer
{
    function_pointer_initializer()
    {
        // First, let's check for the max supported cpuid function
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        cpuid(eax, ebx, ecx, edx);

        const uint32_t max_cpuid_function = eax;
        if (max_cpuid_function >= 1)
        {
            eax = 1;
            ebx = ecx = edx = 0;
            cpuid(eax, ebx, ecx, edx);

            // Check for SSSE3 support
            if (ecx & (1u << 9))
                enable_ssse3();

#if defined(BOOST_LOG_USE_AVX2)
            if (max_cpuid_function >= 7)
            {
                // To check for AVX2 availability we also need to verify that OS supports it
                // Check that OSXSAVE is supported by CPU
                if (ecx & (1u << 27))
                {
                    // Check that it is used by the OS
                    bool mmstate = false;
#if defined(__GNUC__)
                    // Get the XFEATURE_ENABLED_MASK register
                    __asm__ __volatile__
                    (
                        "xgetbv\n\t"
                            : "=a" (eax), "=d" (edx)
                            : "c" (0)
                    );
                    mmstate = (eax & 6U) == 6U;
#elif defined(BOOST_WINDOWS)
                    // MSVC does not have an intrinsic for xgetbv, we have to query OS
                    boost::winapi::HMODULE_ hKernel32 = boost::winapi::GetModuleHandleW(L"kernel32.dll");
                    if (hKernel32)
                    {
                        typedef uint64_t (BOOST_WINAPI_WINAPI_CC* get_enabled_extended_features_t)(uint64_t);
                        get_enabled_extended_features_t get_enabled_extended_features = (get_enabled_extended_features_t)boost::winapi::get_proc_address(hKernel32, "GetEnabledExtendedFeatures");
                        if (get_enabled_extended_features)
                        {
                            // XSTATE_MASK_LEGACY_SSE | XSTATE_MASK_GSSE == 6
                            mmstate = get_enabled_extended_features(6u) == 6u;
                        }
                    }
#endif

                    if (mmstate)
                    {
                        // Finally, check for AVX2 support in CPU
                        eax = 7;
                        ebx = ecx = edx = 0;
                        cpuid(eax, ebx, ecx, edx);

                        if (ebx & (1U << 5))
                            enable_avx2();
                    }
                }
            }
#endif // defined(BOOST_LOG_USE_AVX2)
        }
    }

private:
    static void cpuid(uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx)
    {
#if defined(__GNUC__)
#if (defined(__i386__) || defined(__VXWORKS__)) && (defined(__PIC__) || defined(__PIE__)) && !(defined(__clang__) || (defined(BOOST_GCC) && BOOST_GCC >= 50100))
        // Unless the compiler can do it automatically, we have to backup ebx in 32-bit PIC/PIE code because it is reserved by the ABI.
        // For VxWorks ebx is reserved on 64-bit as well.
#if defined(__x86_64__)
        uint64_t rbx = ebx;
        __asm__ __volatile__
        (
            "xchgq %%rbx, %0\n\t"
            "cpuid\n\t"
            "xchgq %%rbx, %0\n\t"
                : "+DS" (rbx), "+a" (eax), "+c" (ecx), "+d" (edx)
        );
        ebx = static_cast< uint32_t >(rbx);
#else // defined(__x86_64__)
        __asm__ __volatile__
        (
            "xchgl %%ebx, %0\n\t"
            "cpuid\n\t"
            "xchgl %%ebx, %0\n\t"
                : "+DS" (ebx), "+a" (eax), "+c" (ecx), "+d" (edx)
        );
#endif // defined(__x86_64__)
#else
        __asm__ __volatile__
        (
            "cpuid\n\t"
                : "+a" (eax), "+b" (ebx), "+c" (ecx), "+d" (edx)
        );
#endif
#elif defined(_MSC_VER)
        int regs[4] = {};
        __cpuid(regs, eax);
        eax = regs[0];
        ebx = regs[1];
        ecx = regs[2];
        edx = regs[3];
#else
#error Boost.Log: Unexpected compiler
#endif
    }

    static void enable_ssse3()
    {
        dump_data_char = &dump_data_char_ssse3;
        dump_data_wchar = &dump_data_wchar_ssse3;
#if !defined(BOOST_NO_CXX11_CHAR16_T)
        dump_data_char16 = &dump_data_char16_ssse3;
#endif
#if !defined(BOOST_NO_CXX11_CHAR32_T)
        dump_data_char32 = &dump_data_char32_ssse3;
#endif
    }

#if defined(BOOST_LOG_USE_AVX2)
    static void enable_avx2()
    {
        dump_data_char = &dump_data_char_avx2;
        dump_data_wchar = &dump_data_wchar_avx2;
#if !defined(BOOST_NO_CXX11_CHAR16_T)
        dump_data_char16 = &dump_data_char16_avx2;
#endif
#if !defined(BOOST_NO_CXX11_CHAR32_T)
        dump_data_char32 = &dump_data_char32_avx2;
#endif
    }
#endif // defined(BOOST_LOG_USE_AVX2)
};

static function_pointer_initializer g_function_pointer_initializer;

} // namespace

#endif // defined(BOOST_LOG_USE_SSSE3) || defined(BOOST_LOG_USE_AVX2)

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
