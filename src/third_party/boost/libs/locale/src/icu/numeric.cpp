//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/formatting.hpp>
#include "all_generator.hpp"
#include "cdata.hpp"
#include "formatter.hpp"
#include "formatters_cache.hpp"
#include <algorithm>
#include <ios>
#include <limits>
#include <locale>
#include <string>
#include <type_traits>

namespace boost { namespace locale { namespace impl_icu {

    namespace detail {
        template<typename T, typename PreferredType, typename AlternativeType>
        struct choose_type_by_digits
            : std::conditional<std::numeric_limits<T>::digits <= std::numeric_limits<PreferredType>::digits,
                               PreferredType,
                               AlternativeType> {};

        template<typename T, bool integer = std::numeric_limits<T>::is_integer>
        struct icu_format_type {
            static_assert(sizeof(T) <= sizeof(int64_t), "Only up to 64 bit integer types are supported by ICU");
            // ICU supports (only) int32_t and int64_t, use the former as long as it fits, else the latter
            using large_type = typename choose_type_by_digits<T, int64_t, uint64_t>::type;
            using type = typename choose_type_by_digits<T, int32_t, large_type>::type;
        };
        template<typename T>
        struct icu_format_type<T, false> {
            // Only float type ICU supports is double
            using type = double;
        };

        template<typename ValueType>
        static bool use_parent(std::ios_base& ios)
        {
            const uint64_t flg = ios_info::get(ios).display_flags();
            if(flg == flags::posix)
                return true;

            if(!std::numeric_limits<ValueType>::is_integer)
                return false;

            if(flg == flags::number && (ios.flags() & std::ios_base::basefield) != std::ios_base::dec)
                return true;
            return false;
        }
    } // namespace detail

    template<typename CharType>
    class num_format : public std::num_put<CharType> {
    public:
        typedef typename std::num_put<CharType>::iter_type iter_type;
        typedef std::basic_string<CharType> string_type;
        typedef formatter<CharType> formatter_type;

        num_format(const cdata& d, size_t refs = 0) : std::num_put<CharType>(refs), loc_(d.locale()), enc_(d.encoding())
        {}

    protected:
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, long val) const override
        {
            return do_real_put(out, ios, fill, val);
        }
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, unsigned long val) const override
        {
            return do_real_put(out, ios, fill, val);
        }
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, double val) const override
        {
            return do_real_put(out, ios, fill, val);
        }
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, long double val) const override
        {
            return do_real_put(out, ios, fill, val);
        }

        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, long long val) const override
        {
            return do_real_put(out, ios, fill, val);
        }
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, unsigned long long val) const override
        {
            return do_real_put(out, ios, fill, val);
        }

    private:
        template<typename ValueType>
        iter_type do_real_put(iter_type out, std::ios_base& ios, CharType fill, ValueType val) const
        {
            if(detail::use_parent<ValueType>(ios))
                return std::num_put<CharType>::do_put(out, ios, fill, val);

            const std::unique_ptr<formatter_type> formatter = formatter_type::create(ios, loc_, enc_);

            if(!formatter)
                return std::num_put<CharType>::do_put(out, ios, fill, val);

            using icu_type = typename detail::icu_format_type<ValueType>::type;
            size_t code_points;
            const string_type str = formatter->format(static_cast<icu_type>(val), code_points);

            std::streamsize on_left = 0, on_right = 0, points = code_points;
            if(points < ios.width()) {
                const std::streamsize n = ios.width() - points;

                const std::ios_base::fmtflags flags = ios.flags() & std::ios_base::adjustfield;

                // We do not really know internal point, so we assume that it does not
                // exist. So according to the standard field should be right aligned
                if(flags != std::ios_base::left)
                    on_left = n;
                on_right = n - on_left;
            }
            while(on_left > 0) {
                *out++ = fill;
                on_left--;
            }
            std::copy(str.begin(), str.end(), out);
            while(on_right > 0) {
                *out++ = fill;
                on_right--;
            }
            ios.width(0);
            return out;
        }

        icu::Locale loc_;
        std::string enc_;

    }; // num_format

    template<typename CharType>
    class num_parse : public std::num_get<CharType> {
    public:
        num_parse(const cdata& d, size_t refs = 0) : std::num_get<CharType>(refs), loc_(d.locale()), enc_(d.encoding())
        {}

    protected:
        typedef typename std::num_get<CharType>::iter_type iter_type;
        typedef std::basic_string<CharType> string_type;
        typedef formatter<CharType> formatter_type;
        typedef std::basic_istream<CharType> stream_type;

        iter_type
        do_get(iter_type in, iter_type end, std::ios_base& ios, std::ios_base::iostate& err, long& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         unsigned short& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         unsigned int& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         unsigned long& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type
        do_get(iter_type in, iter_type end, std::ios_base& ios, std::ios_base::iostate& err, float& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type
        do_get(iter_type in, iter_type end, std::ios_base& ios, std::ios_base::iostate& err, double& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         long double& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         long long& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         unsigned long long& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

    private:
        //
        // This is not really an efficient solution, but it works
        //
        template<typename ValueType>
        iter_type
        do_real_get(iter_type in, iter_type end, std::ios_base& ios, std::ios_base::iostate& err, ValueType& val) const
        {
            stream_type* stream_ptr = dynamic_cast<stream_type*>(&ios);
            if(!stream_ptr || detail::use_parent<ValueType>(ios))
                return std::num_get<CharType>::do_get(in, end, ios, err, val);

            const std::unique_ptr<formatter_type> formatter = formatter_type::create(ios, loc_, enc_);
            if(!formatter)
                return std::num_get<CharType>::do_get(in, end, ios, err, val);

            string_type tmp;
            tmp.reserve(64);

            CharType c;
            while(in != end && (((c = *in) <= 32 && (c > 0)) || c == 127)) // Assuming that ASCII is a subset
                ++in;

            while(tmp.size() < 4096 && in != end && *in != '\n')
                tmp += *in++;

            using icu_type = typename detail::icu_format_type<ValueType>::type;
            icu_type value;
            size_t parsed_chars;

            if((parsed_chars = formatter->parse(tmp, value)) == 0 || !is_losless_castable<ValueType>(value))
                err |= std::ios_base::failbit;
            else
                val = static_cast<ValueType>(value);

            for(size_t n = tmp.size(); n > parsed_chars; n--)
                stream_ptr->putback(tmp[n - 1]);

            in = iter_type(*stream_ptr);

            if(in == end)
                err |= std::ios_base::eofbit;
            return in;
        }

        BOOST_LOCALE_START_CONST_CONDITION
        template<typename TargetType, typename SrcType>
        bool is_losless_castable(SrcType v) const
        {
            typedef std::numeric_limits<TargetType> target_limits;
            typedef std::numeric_limits<SrcType> casted_limits;
            if(v < 0 && !target_limits::is_signed)
                return false;

            constexpr TargetType max_val = target_limits::max();

            if(sizeof(SrcType) > sizeof(TargetType) && v > static_cast<SrcType>(max_val))
                return false;

            if(target_limits::is_integer == casted_limits::is_integer)
                return true;

            if(target_limits::is_integer) { // and source is not
                if(static_cast<SrcType>(static_cast<TargetType>(v)) != v)
                    return false;
            }
            return true;
        }
        BOOST_LOCALE_END_CONST_CONDITION

        icu::Locale loc_;
        std::string enc_;
    };

    template<typename CharType>
    std::locale install_formatting_facets(const std::locale& in, const cdata& cd)
    {
        std::locale tmp = std::locale(in, new num_format<CharType>(cd));
        if(!std::has_facet<formatters_cache>(in))
            tmp = std::locale(tmp, new formatters_cache(cd.locale()));
        return tmp;
    }

    template<typename CharType>
    std::locale install_parsing_facets(const std::locale& in, const cdata& cd)
    {
        std::locale tmp = std::locale(in, new num_parse<CharType>(cd));
        if(!std::has_facet<formatters_cache>(in))
            tmp = std::locale(tmp, new formatters_cache(cd.locale()));
        return tmp;
    }

    std::locale create_formatting(const std::locale& in, const cdata& cd, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return install_formatting_facets<char>(in, cd);
            case char_facet_t::wchar_f: return install_formatting_facets<wchar_t>(in, cd);
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return install_formatting_facets<char16_t>(in, cd);
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return install_formatting_facets<char32_t>(in, cd);
#endif
        }
        return in;
    }

    std::locale create_parsing(const std::locale& in, const cdata& cd, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return install_parsing_facets<char>(in, cd);
            case char_facet_t::wchar_f: return install_parsing_facets<wchar_t>(in, cd);
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return install_parsing_facets<char16_t>(in, cd);
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return install_parsing_facets<char32_t>(in, cd);
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_icu

// boostinspect:nominmax
