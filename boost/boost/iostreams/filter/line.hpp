// (C) Copyright Jonathan Turkanis 2005.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

#ifndef BOOST_IOSTREAMS_LINE_FILTER_HPP_INCLUDED
#define BOOST_IOSTREAMS_LINE_FILTER_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <algorithm>                               // min.
#include <cassert>
#include <memory>                                  // allocator.
#include <string>
#include <boost/config.hpp>                        // BOOST_STATIC_CONSTANT.
#include <boost/iostreams/categories.hpp>
#include <boost/iostreams/detail/closer.hpp>
#include <boost/iostreams/detail/ios.hpp>          // openmode, streamsize.
#include <boost/iostreams/pipeline.hpp>

// Must come last.
#include <boost/iostreams/detail/config/disable_warnings.hpp> // VC7.1 C4244.

namespace boost { namespace iostreams {

//
// Template name: line_filter.
// Template paramters:
//      Ch - The character type.
//      Alloc - The allocator type.
// Description: Filter which processes data one line at a time.
//
template< typename Ch,
          typename Alloc =
          #if BOOST_WORKAROUND(__GNUC__, < 3)
              typename std::basic_string<Ch>::allocator_type
          #else
              std::allocator<Ch>
          #endif
          >
class basic_line_filter {
private:
    typedef typename std::basic_string<Ch>::traits_type  string_traits;
public:
    typedef Ch                                           char_type;
    typedef char_traits<char_type>                       traits_type;
    typedef std::basic_string<
                Ch,
                string_traits,
                Alloc
            >                                            string_type;
    struct category
        : dual_use,
          filter_tag,
          multichar_tag,
          closable_tag
        { };
protected:
    basic_line_filter() : pos_(string_type::npos), state_(0) { }
public:
    virtual ~basic_line_filter() { }

    template<typename Source>
    std::streamsize read(Source& src, char_type* s, std::streamsize n)
    {
        using namespace std;
        assert(!(state_ & f_write));
        state_ |= f_read;

        // Handle unfinished business.
        streamsize result = 0;
        if (!cur_line_.empty() && (result = read_line(s, n)) == n)
            return n;

        typename traits_type::int_type status = traits_type::good();
        while (result < n && !traits_type::is_eof(status)) {

            // Call next_line() to retrieve a line of filtered test, and
            // read_line() to copy it into buffer s.
            if (traits_type::would_block(status = next_line(src)))
                return result;
            result += read_line(s + result, n - result);
        }

        return detail::check_eof(result);
    }

    template<typename Sink>
    std::streamsize write(Sink& snk, const char_type* s, std::streamsize n)
    {
        using namespace std;
        assert(!(state_ & f_read));
        state_ |= f_write;

        // Handle unfinished business.
        if (pos_ != string_type::npos && !write_line(snk))
            return 0;

        const char_type *cur = s, *next;
        while (true) {

            // Search for the next full line in [cur, s + n), filter it
            // and write it to snk.
            typename string_type::size_type rest = n - (cur - s);
            if ((next = traits_type::find(cur, rest, traits_type::newline()))) {
                cur_line_.append(cur, next - cur);
                cur = next + 1;
                if (!write_line(snk))
                    return static_cast<std::streamsize>(cur - s);
            } else {
                cur_line_.append(cur, rest);
                return n;
            }
        }
    }

    typedef basic_line_filter<Ch, Alloc> self;
    friend struct detail::closer<self>;

    template<typename Sink>
    void close(Sink& snk, BOOST_IOS::openmode which)
    {
        if ((state_ & f_read) && (which & BOOST_IOS::in))
            close();

        if ((state_ & f_write) && (which & BOOST_IOS::out)) {
            detail::closer<self> closer(*this);
            if (!cur_line_.empty())
                write_line(snk);
        }
    }
private:
    virtual string_type do_filter(const string_type& line) = 0;

    // Copies filtered characters fron the current line into
    // the given buffer.
    std::streamsize read_line(char_type* s, std::streamsize n)
    {
        using namespace std;
        streamsize result =
            (std::min) (n, static_cast<streamsize>(cur_line_.size()));
        traits_type::copy(s, cur_line_.data(), result);
        cur_line_.erase(0, result);
        return result;
    }

    // Attempts to retrieve a line of text from the given source; returns
    // an int_type as a good/eof/would_block status code.
    template<typename Source>
    typename traits_type::int_type next_line(Source& src)
    {
        using namespace std;
        typename traits_type::int_type c;
        while ( traits_type::is_good(c = iostreams::get(src)) &&
                c != traits_type::newline() )
        {
            cur_line_ += traits_type::to_int_type(c);
        }
        if (!traits_type::would_block(c)) {
            if (!cur_line_.empty() || c == traits_type::newline())
                cur_line_ = do_filter(cur_line_);
            if (c == traits_type::newline())
                cur_line_ += c;
        }
        return c; // status indicator.
    }

    // Filters the current line and attemps to write it to the given sink.
    // Returns true for success.
    template<typename Sink>
    bool write_line(Sink& snk)
    {
        string_type line = do_filter(cur_line_) + traits_type::newline();
        std::streamsize amt = static_cast<std::streamsize>(line.size());
        bool result = iostreams::write(snk, line.data(), amt) == amt;
        if (result)
            clear();
        return result;
    }

    void close()
    {
        clear();
        state_ = 0;
    }

    void clear()
    {
        cur_line_.erase();
        pos_ = string_type::npos;
    }

    enum flag_type {
        f_read   = 1,
        f_write  = f_read << 1
    };

    string_type                      cur_line_;
    typename string_type::size_type  pos_;
    int                              state_;
};
BOOST_IOSTREAMS_PIPABLE(basic_line_filter, 2)

typedef basic_line_filter<char>     line_filter;
typedef basic_line_filter<wchar_t>  wline_filter;

} } // End namespaces iostreams, boost.

#include <boost/iostreams/detail/config/enable_warnings.hpp>

#endif // #ifndef BOOST_IOSTREAMS_LINE_FILTER_HPP_INCLUDED
