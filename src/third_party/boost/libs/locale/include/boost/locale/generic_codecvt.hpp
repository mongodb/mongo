//
// Copyright (c) 2015 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_GENERIC_CODECVT_HPP
#define BOOST_LOCALE_GENERIC_CODECVT_HPP

#include <boost/locale/utf.hpp>
#include <cstdint>
#include <locale>

namespace boost { namespace locale {

    static_assert(sizeof(std::mbstate_t) >= 2, "std::mbstate_t is to small to store an UTF-16 codepoint");
    namespace detail {
        // Avoid including cstring for std::memcpy
        inline void copy_uint16_t(void* dst, const void* src)
        {
            unsigned char* cdst = static_cast<unsigned char*>(dst);
            const unsigned char* csrc = static_cast<const unsigned char*>(src);
            cdst[0] = csrc[0];
            cdst[1] = csrc[1];
        }
        inline uint16_t read_state(const std::mbstate_t& src)
        {
            uint16_t dst;
            copy_uint16_t(&dst, &src);
            return dst;
        }
        inline void write_state(std::mbstate_t& dst, const uint16_t src)
        {
            copy_uint16_t(&dst, &src);
        }
    } // namespace detail

    /// \brief A base class that used to define constants for generic_codecvt
    class generic_codecvt_base {
    public:
        /// Initial state for converting to or from Unicode code points, used by initial_state in derived classes
        enum initial_convertion_state {
            to_unicode_state,  ///< The state would be used by to_unicode functions
            from_unicode_state ///< The state would be used by from_unicode functions
        };
    };

    /// \brief Generic codecvt facet for various stateless encodings to UTF-16 and UTF-32 using wchar_t, char32_t
    /// and char16_t
    ///
    /// Implementations should derive from this class defining itself as CodecvtImpl and provide following members
    ///
    /// - `state_type` - a type of special object that allows to store intermediate cached data, for example `iconv_t`
    /// descriptor
    /// - `state_type initial_state(generic_codecvt_base::initial_convertion_state direction) const` - member function
    /// that creates initial state
    /// - `int max_encoding_length() const` - a maximal length that one Unicode code point is represented, for UTF-8 for
    /// example it is 4 from ISO-8859-1 it is 1
    /// - `utf::code_point to_unicode(state_type& state, const char*& begin, const char* end)` - extract first code
    /// point from the text in range [begin,end), in case of success begin would point to the next character sequence to
    /// be encoded to next code point, in case of incomplete sequence - utf::incomplete shell be returned, and in case
    /// of invalid input sequence utf::illegal shell be returned and begin would remain unmodified
    /// - `utf::len_or_error from_unicode(state_type &state, utf::code_point u, char* begin, const char* end)` - convert
    /// a Unicode code point `u` into a character sequence at [begin,end). Return the length of the sequence in case of
    /// success, utf::incomplete in case of not enough room to encode the code point, or utf::illegal in case conversion
    /// can not be performed
    ///
    ///
    /// For example implementation of codecvt for latin1/ISO-8859-1 character set
    ///
    /// \code
    ///
    /// template<typename CharType>
    /// class latin1_codecvt: boost::locale::generic_codecvt<CharType,latin1_codecvt<CharType> >
    /// {
    /// public:
    ///
    ///     /* Standard codecvt constructor */
    ///     latin1_codecvt(size_t refs = 0): boost::locale::generic_codecvt<CharType,latin1_codecvt<CharType> >(refs)
    ///     {
    ///     }
    ///
    ///     /* State is unused but required by generic_codecvt */
    ///     struct state_type {};
    ///
    ///     state_type initial_state(generic_codecvt_base::initial_convertion_state /*unused*/) const
    ///     {
    ///         return state_type();
    ///     }
    ///
    ///     int max_encoding_length() const
    ///     {
    ///         return 1;
    ///     }
    ///
    ///     boost::locale::utf::code_point to_unicode(state_type&, const char*& begin, const char* end) const
    ///     {
    ///        if(begin == end)
    ///           return boost::locale::utf::incomplete;
    ///        return *begin++;
    ///     }
    ///
    ///     boost::locale::utf::len_or_error from_unicode(state_type&, boost::locale::utf::code_point u,
    ///                                                   char* begin, const char* end) const
    ///     {
    ///        if(u >= 256)
    ///           return boost::locale::utf::illegal;
    ///        if(begin == end)
    ///           return boost::locale::utf::incomplete;
    ///        *begin = u;
    ///        return 1;
    ///     }
    /// };
    ///
    /// \endcode
    ///
    /// When external tools used for encoding conversion, the `state_type` is useful to save objects used for
    /// conversions. For example, icu::UConverter can be saved in such a state for an efficient use:
    ///
    /// \code
    /// template<typename CharType>
    /// class icu_codecvt: boost::locale::generic_codecvt<CharType,icu_codecvt<CharType>>
    /// {
    /// public:
    ///
    ///     /* Standard codecvt constructor */
    ///     icu_codecvt(std::string const &name,refs = 0):
    ///         boost::locale::generic_codecvt<CharType,icu_codecvt<CharType>>(refs)
    ///     { ... }
    ///
    ///     using state_type = std::unique_ptr<UConverter,void (*)(UConverter*)>;
    ///
    ///     state_type initial_state(generic_codecvt_base::initial_convertion_state /*unused*/) const
    ///     {
    ///         UErrorCode err = U_ZERO_ERROR;
    ///         return state_type(ucnv_safeClone(converter_,0,0,&err),ucnv_close);
    ///     }
    ///
    ///     boost::locale::utf::code_point to_unicode(state_type &ptr,char const *&begin,char const *end) const
    ///     {
    ///         UErrorCode err = U_ZERO_ERROR;
    ///         boost::locale::utf::code_point cp = ucnv_getNextUChar(ptr.get(),&begin,end,&err);
    ///         ...
    ///     }
    ///     ...
    /// };
    /// \endcode
    ///
    template<typename CharType, typename CodecvtImpl, int CharSize = sizeof(CharType)>
    class generic_codecvt;

    /// \brief UTF-16 to/from narrow char codecvt facet to use with char16_t or wchar_t on Windows
    ///
    /// Note in order to fit the requirements of usability by std::wfstream it uses mbstate_t
    /// to handle intermediate states in handling of variable length UTF-16 sequences
    ///
    /// Its member functions implement standard virtual functions of basic codecvt
    template<typename CharType, typename CodecvtImpl>
    class generic_codecvt<CharType, CodecvtImpl, 2> : public std::codecvt<CharType, char, std::mbstate_t>,
                                                      public generic_codecvt_base {
    public:
        typedef CharType uchar;

        generic_codecvt(size_t refs = 0) : std::codecvt<CharType, char, std::mbstate_t>(refs) {}
        const CodecvtImpl& implementation() const { return *static_cast<const CodecvtImpl*>(this); }

    protected:
        std::codecvt_base::result do_unshift(std::mbstate_t& s, char* from, char* /*to*/, char*& next) const override
        {
            if(*reinterpret_cast<char*>(&s) != 0)
                return std::codecvt_base::error;
            next = from;
            return std::codecvt_base::ok;
        }
        int do_encoding() const noexcept override { return 0; }
        int do_max_length() const noexcept override { return implementation().max_encoding_length(); }
        bool do_always_noconv() const noexcept override { return false; }

        int do_length(std::mbstate_t& std_state, const char* from, const char* from_end, size_t max) const override
        {
            bool state = *reinterpret_cast<char*>(&std_state) != 0;
            const char* save_from = from;

            auto cvt_state = implementation().initial_state(to_unicode_state);
            while(max > 0 && from < from_end) {
                const char* prev_from = from;
                const utf::code_point ch = implementation().to_unicode(cvt_state, from, from_end);
                if(ch == boost::locale::utf::incomplete || ch == boost::locale::utf::illegal) {
                    from = prev_from;
                    break;
                }
                max--;
                if(ch > 0xFFFF) {
                    if(!state)
                        from = prev_from;
                    state = !state;
                }
            }
            *reinterpret_cast<char*>(&std_state) = state;
            return static_cast<int>(from - save_from);
        }

        std::codecvt_base::result do_in(std::mbstate_t& std_state,
                                        const char* from,
                                        const char* from_end,
                                        const char*& from_next,
                                        uchar* to,
                                        uchar* to_end,
                                        uchar*& to_next) const override
        {
            std::codecvt_base::result r = std::codecvt_base::ok;

            // mbstate_t is POD type and should be initialized to 0 (i.a. state = stateT())
            // according to standard. We use it to keep a flag 0/1 for surrogate pair writing
            //
            // if 0/false no codepoint above >0xFFFF observed, else a codepoint above 0xFFFF was observed
            // and first pair is written, but no input consumed
            bool state = *reinterpret_cast<char*>(&std_state) != 0;
            auto cvt_state = implementation().initial_state(to_unicode_state);
            while(to < to_end && from < from_end) {
                const char* from_saved = from;

                utf::code_point ch = implementation().to_unicode(cvt_state, from, from_end);

                if(ch == boost::locale::utf::illegal) {
                    from = from_saved;
                    r = std::codecvt_base::error;
                    break;
                }
                if(ch == boost::locale::utf::incomplete) {
                    from = from_saved;
                    r = std::codecvt_base::partial;
                    break;
                }
                // Normal codepoints go directly to stream
                if(ch <= 0xFFFF)
                    *to++ = static_cast<uchar>(ch);
                else {
                    // For other codepoints we do the following
                    //
                    // 1. We can't consume our input as we may find ourselves
                    //    in state where all input consumed but not all output written,i.e. only
                    //    1st pair is written
                    // 2. We only write first pair and mark this in the state, we also revert back
                    //    the from pointer in order to make sure this codepoint would be read
                    //    once again and then we would consume our input together with writing
                    //    second surrogate pair
                    ch -= 0x10000;
                    std::uint16_t w1 = static_cast<std::uint16_t>(0xD800 | (ch >> 10));
                    std::uint16_t w2 = static_cast<std::uint16_t>(0xDC00 | (ch & 0x3FF));
                    if(!state) {
                        from = from_saved;
                        *to++ = w1;
                    } else
                        *to++ = w2;
                    state = !state;
                }
            }
            from_next = from;
            to_next = to;
            if(r == std::codecvt_base::ok && (from != from_end || state))
                r = std::codecvt_base::partial;
            *reinterpret_cast<char*>(&std_state) = state;
            return r;
        }

        std::codecvt_base::result do_out(std::mbstate_t& std_state,
                                         const uchar* from,
                                         const uchar* from_end,
                                         const uchar*& from_next,
                                         char* to,
                                         char* to_end,
                                         char*& to_next) const override
        {
            std::codecvt_base::result r = std::codecvt_base::ok;
            // mbstate_t is POD type and should be initialized to 0 (i.a. state = stateT())
            // according to standard. We assume that sizeof(mbstate_t) >=2 in order
            // to be able to store first observed surrogate pair
            //
            // State: state!=0 - a first surrogate pair was observed (state = first pair),
            // we expect the second one to come and then zero the state
            std::uint16_t state = detail::read_state(std_state);
            auto cvt_state = implementation().initial_state(from_unicode_state);
            while(to < to_end && from < from_end) {
                utf::code_point ch = 0;
                if(state != 0) {
                    // if the state indicates that 1st surrogate pair was written
                    // we should make sure that the second one that comes is actually
                    // second surrogate
                    std::uint16_t w1 = state;
                    std::uint16_t w2 = *from;
                    // we don't forward from as writing may fail to incomplete or
                    // partial conversion
                    if(0xDC00 <= w2 && w2 <= 0xDFFF) {
                        std::uint16_t vh = w1 - 0xD800;
                        std::uint16_t vl = w2 - 0xDC00;
                        ch = ((uint32_t(vh) << 10) | vl) + 0x10000;
                    } else {
                        // Invalid surrogate
                        r = std::codecvt_base::error;
                        break;
                    }
                } else {
                    ch = *from;
                    if(0xD800 <= ch && ch <= 0xDBFF) {
                        // if this is a first surrogate pair we put
                        // it into the state and consume it, note we don't
                        // go forward as it should be illegal so we increase
                        // the from pointer manually
                        state = static_cast<uint16_t>(ch);
                        from++;
                        continue;
                    } else if(0xDC00 <= ch && ch <= 0xDFFF) {
                        // if we observe second surrogate pair and
                        // first only may be expected we should break from the loop with error
                        // as it is illegal input
                        r = std::codecvt_base::error;
                        break;
                    }
                }
                if(!boost::locale::utf::is_valid_codepoint(ch)) {
                    r = std::codecvt_base::error;
                    break;
                }
                const utf::code_point len = implementation().from_unicode(cvt_state, ch, to, to_end);
                if(len == boost::locale::utf::incomplete) {
                    r = std::codecvt_base::partial;
                    break;
                } else if(len == boost::locale::utf::illegal) {
                    r = std::codecvt_base::error;
                    break;
                } else
                    to += len;
                state = 0;
                from++;
            }
            from_next = from;
            to_next = to;
            if(r == std::codecvt_base::ok && (from != from_end || state != 0))
                r = std::codecvt_base::partial;
            detail::write_state(std_state, state);
            return r;
        }
    };

    /// \brief UTF-32 to/from narrow char codecvt facet to use with char32_t or wchar_t on POSIX platforms
    ///
    /// Its member functions implement standard virtual functions of basic codecvt.
    /// mbstate_t is not used for UTF-32 handling due to fixed length encoding
    template<typename CharType, typename CodecvtImpl>
    class generic_codecvt<CharType, CodecvtImpl, 4> : public std::codecvt<CharType, char, std::mbstate_t>,
                                                      public generic_codecvt_base {
    public:
        typedef CharType uchar;

        generic_codecvt(size_t refs = 0) : std::codecvt<CharType, char, std::mbstate_t>(refs) {}

        const CodecvtImpl& implementation() const { return *static_cast<const CodecvtImpl*>(this); }

    protected:
        std::codecvt_base::result
        do_unshift(std::mbstate_t& /*s*/, char* from, char* /*to*/, char*& next) const override
        {
            next = from;
            return std::codecvt_base::ok;
        }
        int do_encoding() const noexcept override { return 0; }
        int do_max_length() const noexcept override { return implementation().max_encoding_length(); }
        bool do_always_noconv() const noexcept override { return false; }

        int do_length(std::mbstate_t& /*state*/, const char* from, const char* from_end, size_t max) const override
        {
            const char* start_from = from;
            auto cvt_state = implementation().initial_state(to_unicode_state);
            while(max > 0 && from < from_end) {
                const char* save_from = from;
                const utf::code_point ch = implementation().to_unicode(cvt_state, from, from_end);
                if(ch == boost::locale::utf::incomplete || ch == boost::locale::utf::illegal) {
                    from = save_from;
                    break;
                }
                max--;
            }

            return static_cast<int>(from - start_from);
        }

        std::codecvt_base::result do_in(std::mbstate_t& /*state*/,
                                        const char* from,
                                        const char* from_end,
                                        const char*& from_next,
                                        uchar* to,
                                        uchar* to_end,
                                        uchar*& to_next) const override
        {
            std::codecvt_base::result r = std::codecvt_base::ok;

            auto cvt_state = implementation().initial_state(to_unicode_state);
            while(to < to_end && from < from_end) {
                const char* from_saved = from;

                const utf::code_point ch = implementation().to_unicode(cvt_state, from, from_end);

                if(ch == boost::locale::utf::illegal) {
                    r = std::codecvt_base::error;
                    from = from_saved;
                    break;
                }
                if(ch == boost::locale::utf::incomplete) {
                    r = std::codecvt_base::partial;
                    from = from_saved;
                    break;
                }
                *to++ = ch;
            }
            from_next = from;
            to_next = to;
            if(r == std::codecvt_base::ok && from != from_end)
                r = std::codecvt_base::partial;
            return r;
        }

        std::codecvt_base::result do_out(std::mbstate_t& /*std_state*/,
                                         const uchar* from,
                                         const uchar* from_end,
                                         const uchar*& from_next,
                                         char* to,
                                         char* to_end,
                                         char*& to_next) const override
        {
            std::codecvt_base::result r = std::codecvt_base::ok;
            auto cvt_state = implementation().initial_state(from_unicode_state);
            while(to < to_end && from < from_end) {
                const std::uint32_t ch = *from;
                if(!boost::locale::utf::is_valid_codepoint(ch)) {
                    r = std::codecvt_base::error;
                    break;
                }
                const utf::code_point len = implementation().from_unicode(cvt_state, ch, to, to_end);
                if(len == boost::locale::utf::incomplete) {
                    r = std::codecvt_base::partial;
                    break;
                } else if(len == boost::locale::utf::illegal) {
                    r = std::codecvt_base::error;
                    break;
                }
                to += len;
                from++;
            }
            from_next = from;
            to_next = to;
            if(r == std::codecvt_base::ok && from != from_end)
                r = std::codecvt_base::partial;
            return r;
        }
    };

    template<typename CodecvtImpl>
    class generic_codecvt<char, CodecvtImpl, 1> : public std::codecvt<char, char, std::mbstate_t>,
                                                  public generic_codecvt_base {
    public:
        typedef char uchar;

        const CodecvtImpl& implementation() const { return *static_cast<const CodecvtImpl*>(this); }

        generic_codecvt(size_t refs = 0) : std::codecvt<char, char, std::mbstate_t>(refs) {}
    };

}} // namespace boost::locale

#endif
