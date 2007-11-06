// (C) Copyright Jonathan Turkanis 2003.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

// Contains the definitions of the class templates gzip_compressor and
// gzip_decompressor for reading and writing files in the gzip file format
// (RFC 1952). Based in part on work of Jonathan de Halleux; see [...]

#ifndef BOOST_IOSTREAMS_GZIP_HPP_INCLUDED
#define BOOST_IOSTREAMS_GZIP_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp> // STATIC_CONSTANT, STDC_NAMESPACE, 
                            // DINKUMWARE_STDLIB, __STL_CONFIG_H.
#include <algorithm>                      // min.
#include <cstdio>                         // EOF.
#include <cstddef>                        // size_t.
#include <ctime>                          // std::time_t.
#include <memory>                         // allocator.
#include <boost/config.hpp>               // Put size_t in std.
#include <boost/detail/workaround.hpp>
#include <boost/cstdint.hpp>              // uint8_t, uint32_t.
#include <boost/iostreams/constants.hpp>  // buffer size.
#include <boost/iostreams/detail/adapter/non_blocking_adapter.hpp>
#include <boost/iostreams/detail/adapter/range_adapter.hpp>
#include <boost/iostreams/detail/char_traits.hpp>
#include <boost/iostreams/detail/ios.hpp> // failure.
#include <boost/iostreams/operations.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/pipeline.hpp>         

// Must come last.
#if defined(BOOST_MSVC)
# pragma warning(push)
# pragma warning(disable: 4309)    // Truncation of constant value.
#endif

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::time_t; }
#endif

namespace boost { namespace iostreams {

namespace gzip {

using namespace boost::iostreams::zlib;

    // Error codes used by gzip_error.

const int zlib_error        = 1;
const int bad_crc           = 2; // Recorded crc doesn't match data.
const int bad_length        = 3; // Recorded length doesn't match data.
const int bad_header        = 4; // Malformed header.
const int bad_footer        = 5; // Malformed footer.

namespace magic {

    // Magic numbers used by gzip header.

const int id1               = 0x1f;
const int id2               = 0x8b;

} // End namespace magic.

namespace method {

    // Codes used for the 'CM' byte of the gzip header.

const int deflate           = 8;

} // End namespace method.

namespace flags {

    // Codes used for the 'FLG' byte of the gzip header.

const int text              = 1;
const int header_crc        = 2;
const int extra             = 4;
const int name              = 8;
const int comment           = 16;

} // End namespace flags.

namespace extra_flags {

    // Codes used for the 'XFL' byte of the gzip header.

const int best_compression  = 2;
const int best_speed        = 4;

} // End namespace extra_flags.

    // Codes used for the 'OS' byte of the gzip header.

const int os_fat            = 0;
const int os_amiga          = 1;
const int os_vms            = 2;
const int os_unix           = 3;
const int os_vm_cms         = 4;
const int os_atari          = 5;
const int os_hpfs           = 6;
const int os_macintosh      = 7;
const int os_z_system       = 8;
const int os_cp_m           = 9;
const int os_tops_20        = 10;
const int os_ntfs           = 11;
const int os_qdos           = 12;
const int os_acorn          = 13;
const int os_unknown        = 255;

} // End namespace gzip.

//
// Class name: gzip_params.
// Description: Subclass of zlib_params with an additional field
//      representing a file name.
//
struct gzip_params : zlib_params {

    // Non-explicit constructor.
    gzip_params( int level              = gzip::default_compression,
                 int method             = gzip::deflated,
                 int window_bits        = gzip::default_window_bits,
                 int mem_level          = gzip::default_mem_level,
                 int strategy           = gzip::default_strategy,
                 std::string file_name  = "",
                 std::string comment    = "",
                 std::time_t mtime      = 0 )
        : zlib_params(level, method, window_bits, mem_level, strategy),
          file_name(file_name), mtime(mtime)
        { }
    std::string  file_name;
    std::string  comment;
    std::time_t  mtime;
};

//
// Class name: gzip_error.
// Description: Subclass of std::ios_base::failure thrown to indicate
//     zlib errors other than out-of-memory conditions.
//
class gzip_error : public BOOST_IOSTREAMS_FAILURE {
public:
    explicit gzip_error(int error)
        : BOOST_IOSTREAMS_FAILURE("gzip error"),
          error_(error), zlib_error_code_(zlib::okay) { }
    explicit gzip_error(const zlib_error& e)
        : BOOST_IOSTREAMS_FAILURE("gzip error"),
          error_(gzip::zlib_error), zlib_error_code_(e.error())
        { }
    int error() const { return error_; }
    int zlib_error_code() const { return zlib_error_code_; }
private:
    int error_;
    int zlib_error_code_;
};

//
// Template name: gzip_compressor
// Description: Model of OutputFilter implementing compression in the
//      gzip format.
//
template<typename Alloc = std::allocator<char> >
class basic_gzip_compressor : basic_zlib_compressor<Alloc> {
private:
    typedef basic_zlib_compressor<Alloc>  base_type;
public:
    typedef char char_type;
    struct category
        : dual_use,
          filter_tag,
          multichar_tag,
          closable_tag
        { };
    basic_gzip_compressor( const gzip_params& = gzip::default_compression,
                           int buffer_size = default_device_buffer_size );

    template<typename Source>
    std::streamsize read(Source& src, char_type* s, std::streamsize n)
    {
        using namespace std;
        streamsize result = 0;

        // Read header.
        if (!(flags_ & f_header_done))
            result += read_string(s, n, header_);

        // Read body.
        if (!(flags_ & f_body_done)) {

            // Read from basic_zlib_filter.
            streamsize amt = base_type::read(src, s + result, n - result);
            if (amt != -1) {
                result += amt;
                if (amt < n - result) { // Double-check for EOF.
                    amt = base_type::read(src, s + result, n - result);
                    if (amt != -1)
                        result += amt;
                }
            }
            if (amt == -1)
                prepare_footer();
        }

        // Read footer.
        if ((flags_ & f_body_done) != 0 && result < n)
            result += read_string(s + result, n - result, footer_);

        return result != 0 ? result : -1;
    }

    template<typename Sink>
    std::streamsize write(Sink& snk, const char_type* s, std::streamsize n)
    {
        if (!(flags_ & f_header_done)) {
            std::streamsize amt = 
                static_cast<std::streamsize>(header_.size() - offset_);
            offset_ += boost::iostreams::write(snk, header_.data() + offset_, amt);
            if (offset_ == header_.size())
                flags_ |= f_header_done;
            else
                return 0;
        }
        return base_type::write(snk, s, n);
    }

    template<typename Sink>
    void close(Sink& snk, BOOST_IOS::openmode m)
    {
        namespace io = boost::iostreams;

        if (m & BOOST_IOS::out) {

            // Close zlib compressor.
            base_type::close(snk, BOOST_IOS::out);

            if (flags_ & f_header_done) {

                // Write final fields of gzip file format.
                write_long(this->crc(), snk);
                write_long(this->total_in(), snk);
            }

        }
        #if BOOST_WORKAROUND(__GNUC__, == 2) && defined(__STL_CONFIG_H) || \
            BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, == 1) \
            /**/
            footer_.erase(0, std::string::npos);
        #else
            footer_.clear();
        #endif
        offset_ = 0;
        flags_ = 0;
    }
private:
    static gzip_params normalize_params(gzip_params p);
    void prepare_footer();
    std::streamsize read_string(char* s, std::streamsize n, std::string& str);

    template<typename Sink>
    static void write_long(long n, Sink& next)
    {
        boost::iostreams::put(next, static_cast<char>(0xFF & n));
        boost::iostreams::put(next, static_cast<char>(0xFF & (n >> 8)));
        boost::iostreams::put(next, static_cast<char>(0xFF & (n >> 16)));
        boost::iostreams::put(next, static_cast<char>(0xFF & (n >> 24)));
    }

    enum flag_type {
        f_header_done = 1,
        f_body_done = f_header_done << 1,
        f_footer_done = f_body_done << 1
    };
    std::string  header_;
    std::string  footer_;
    std::size_t  offset_;
    int          flags_;
};
BOOST_IOSTREAMS_PIPABLE(basic_gzip_compressor, 1)

typedef basic_gzip_compressor<> gzip_compressor;

//
// Template name: basic_gzip_decompressor
// Description: Model of InputFilter implementing compression in the
//      gzip format.
//
template<typename Alloc = std::allocator<char> >
class basic_gzip_decompressor : basic_zlib_decompressor<Alloc> {
public:
    typedef char char_type;
    struct category
        : multichar_input_filter_tag,
          closable_tag
        { };
    basic_gzip_decompressor( int window_bits = gzip::default_window_bits,
                             int buffer_size = default_device_buffer_size );

    template<typename Source>
    std::streamsize read(Source& src, char_type* s, std::streamsize n)
    {
        if ((flags_ & f_header_read) == 0) {
            non_blocking_adapter<Source> nb(src);
            read_header(nb);
            flags_ |= f_header_read;
        }

        if ((flags_ & f_footer_read) != 0)
            return -1;
        
        try {
            std::streamsize result = 0;
            std::streamsize amt;
            if ((amt = base_type::read(src, s, n)) != -1) {
                result += amt;
                if (amt < n) { // Double check for EOF.
                    amt = base_type::read(src, s + result, n - result);
                    if (amt != -1)
                        result += amt;
                }
            }
            if (amt == -1) {
                non_blocking_adapter<Source> nb(src);
                read_footer(nb);
                flags_ |= f_footer_read;
            }
            return result;
        } catch (const zlib_error& e) {
            throw gzip_error(e);
        }
    }

    template<typename Source>
    void close(Source& src)
    {
        try {
            base_type::close(src, BOOST_IOS::in);
            flags_ = 0;
        } catch (const zlib_error& e) {
            throw gzip_error(e);
        }
    }

    std::string file_name() const { return file_name_; }
    std::string comment() const { return comment_; }
    bool text() const { return (flags_ & gzip::flags::text) != 0; }
    int os() const { return os_; }
    std::time_t mtime() const { return mtime_; }
private:
    typedef basic_zlib_decompressor<Alloc>     base_type;
    typedef BOOST_IOSTREAMS_CHAR_TRAITS(char)  traits_type;
    static bool is_eof(int c) { return traits_type::eq_int_type(c, EOF); }
    static gzip_params make_params(int window_bits);

    template<typename Source>
    static uint8_t read_uint8(Source& src, int error)
     {
        int c;
        if ((c = boost::iostreams::get(src)) == EOF || c == WOULD_BLOCK)
            throw gzip_error(error);
        return static_cast<uint8_t>(traits_type::to_char_type(c));
    }

    template<typename Source>
    static uint32_t read_uint32(Source& src, int error)
    {
        uint8_t b1 = read_uint8(src, error);
        uint8_t b2 = read_uint8(src, error);
        uint8_t b3 = read_uint8(src, error);
        uint8_t b4 = read_uint8(src, error);
        return b1 + (b2 << 8) + (b3 << 16) + (b4 << 24);
    }

    template<typename Source>
    std::string read_string(Source& src)
    {
        std::string result;
        while (true) {
            int c;
            if (is_eof(c = boost::iostreams::get(src)))
                throw gzip_error(gzip::bad_header);
            else if (c == 0)
                return result;
            else
                result += static_cast<char>(c);
        }
    }

    template<typename Source>
    void read_header(Source& src) // Source is non-blocking.
    {
        // Reset saved values.
        #if BOOST_WORKAROUND(__GNUC__, == 2) && defined(__STL_CONFIG_H) || \
            BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, == 1) \
            /**/
            file_name_.erase(0, std::string::npos);
            comment_.erase(0, std::string::npos);
        #else
            file_name_.clear();
            comment_.clear();
        #endif
        os_ = gzip::os_unknown;
        mtime_ = 0;

        int flags;

        // Read header, without checking header crc.
        if ( boost::iostreams::get(src) != gzip::magic::id1 ||   // ID1.
             boost::iostreams::get(src) != gzip::magic::id2 ||   // ID2.
             is_eof(boost::iostreams::get(src)) ||               // CM.
             is_eof(flags = boost::iostreams::get(src)) )        // FLG.
        {
            throw gzip_error(gzip::bad_header);
        }
        mtime_ = read_uint32(src, gzip::bad_header);        // MTIME.
        read_uint8(src, gzip::bad_header);                 // XFL.
        os_ = read_uint8(src, gzip::bad_header);          // OS.
        if (flags & boost::iostreams::gzip::flags::text)
            flags_ |= f_text;

        // Skip extra field. (From J. Halleaux; see note at top.)
        if (flags & gzip::flags::extra) {
            int length = 
                static_cast<int>(
                    read_uint8(src, gzip::bad_header) +
                    (read_uint8(src, gzip::bad_header) << 8)
                );
            // length is garbage if EOF but the loop below will quit anyway.
            do { }
            while (length-- != 0 && !is_eof(boost::iostreams::get(src)));
        }

        if (flags & gzip::flags::name)          // Read file name.
            file_name_ = read_string(src);
        if (flags & gzip::flags::comment)       // Read comment.
            comment_ = read_string(src);
        if (flags & gzip::flags::header_crc) {  // Skip header crc.
            read_uint8(src, gzip::bad_header);
            read_uint8(src, gzip::bad_header);
        }
    }

    template<typename Source>
    void read_footer(Source& src)
    {
        typename base_type::string_type footer = 
            this->unconsumed_input();
        int c;
        while (!is_eof(c = boost::iostreams::get(src)))
            footer += c;
        detail::range_adapter<input, std::string> 
            rng(footer.begin(), footer.end());
        if (read_uint32(rng, gzip::bad_footer) != this->crc())
            throw gzip_error(gzip::bad_crc);
        if (static_cast<int>(read_uint32(rng, gzip::bad_footer)) != this->total_out())
            throw gzip_error(gzip::bad_length);
    }
    enum flag_type {
        f_header_read  = 1,
        f_footer_read  = f_header_read << 1,
        f_text         = f_footer_read << 1
    };
    std::string  file_name_;
    std::string  comment_;
    int          os_;
    std::time_t  mtime_;
    int          flags_;
};
BOOST_IOSTREAMS_PIPABLE(basic_gzip_decompressor, 1)

typedef basic_gzip_decompressor<> gzip_decompressor;

//------------------Implementation of gzip_compressor-------------------------//

template<typename Alloc>
basic_gzip_compressor<Alloc>::basic_gzip_compressor
    (const gzip_params& p, int buffer_size)
    : base_type(normalize_params(p), buffer_size),
      offset_(0), flags_(0)
{
    // Calculate gzip header.
    bool has_name = !p.file_name.empty();
    bool has_comment = !p.comment.empty();

    std::string::size_type length =
        10 +
        (has_name ? p.file_name.size() + 1 : 0) +
        (has_comment ? p.comment.size() + 1 : 0);
        // + 2; // Header crc confuses gunzip.
    int flags =
        //gzip::flags::header_crc +
        (has_name ? gzip::flags::name : 0) +
        (has_comment ? gzip::flags::comment : 0);
    int extra_flags =
        ( p.level == zlib::best_compression ?
              gzip::extra_flags::best_compression :
              0 ) +
        ( p.level == zlib::best_speed ?
              gzip::extra_flags::best_speed :
              0 );
    header_.reserve(length);
    header_ += gzip::magic::id1;                         // ID1.
    header_ += gzip::magic::id2;                         // ID2.
    header_ += gzip::method::deflate;                    // CM.
    header_ += static_cast<char>(flags);                 // FLG.
    header_ += static_cast<char>(0xFF & p.mtime);        // MTIME.
    header_ += static_cast<char>(0xFF & (p.mtime >> 8));
    header_ += static_cast<char>(0xFF & (p.mtime >> 16));
    header_ += static_cast<char>(0xFF & (p.mtime >> 24));
    header_ += static_cast<char>(extra_flags);           // XFL.
    header_ += static_cast<char>(gzip::os_unknown);      // OS.
    if (has_name) {
        header_ += p.file_name;
        header_ += '\0';
    }
    if (has_comment) {
        header_ += p.comment;
        header_ += '\0';
    }
}

template<typename Alloc>
gzip_params basic_gzip_compressor<Alloc>::normalize_params(gzip_params p)
{
    p.noheader = true;
    p.calculate_crc = true;
    return p;
}

template<typename Alloc>
void basic_gzip_compressor<Alloc>::prepare_footer()
{
    boost::iostreams::back_insert_device<std::string> out(footer_);
    write_long(this->crc(), out);
    write_long(this->total_in(), out);
    flags_ |= f_body_done;
    offset_ = 0;
}

template<typename Alloc>
std::streamsize basic_gzip_compressor<Alloc>::read_string
    (char* s, std::streamsize n, std::string& str)
{
    using namespace std;
    streamsize avail =
        static_cast<streamsize>(str.size() - offset_);
    streamsize amt = (std::min)(avail, n);
    std::copy( str.data() + offset_,
               str.data() + offset_ + amt,
               s );
    offset_ += amt;
    if ( !(flags_ & f_header_done) &&
         offset_ == static_cast<std::size_t>(str.size()) )
    {
        flags_ |= f_header_done;
    }
    return amt;
}

//------------------Implementation of gzip_decompressor-----------------------//

template<typename Alloc>
basic_gzip_decompressor<Alloc>::basic_gzip_decompressor
    (int window_bits, int buffer_size)
    : base_type(make_params(window_bits), buffer_size),
      os_(gzip::os_unknown), mtime_(0), flags_(0)
    { }

template<typename Alloc>
gzip_params basic_gzip_decompressor<Alloc>::make_params(int window_bits)
{
    gzip_params p;
    p.window_bits = window_bits;
    p.noheader = true;
    p.calculate_crc = true;
    return p;
}

//----------------------------------------------------------------------------//

} } // End namespaces iostreams, boost.

#if defined(BOOST_MSVC)
# pragma warning(pop)
#endif

#endif // #ifndef BOOST_IOSTREAMS_GZIP_HPP_INCLUDED
