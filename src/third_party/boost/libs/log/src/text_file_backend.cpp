/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   text_file_backend.cpp
 * \author Andrey Semashev
 * \date   09.06.2009
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <ctime>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <list>
#include <string>
#include <locale>
#include <ostream>
#include <sstream>
#include <iterator>
#include <algorithm>
#include <stdexcept>
#include <boost/core/ref.hpp>
#include <boost/bind/bind.hpp>
#include <boost/cstdint.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/throw_exception.hpp>
#include <boost/mpl/if.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/list_hook.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian_types.hpp>
#include <boost/spirit/home/qi/numeric/numeric_utils.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/detail/light_function.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/attributes/time_traits.hpp>
#include <boost/log/sinks/auto_newline_mode.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include "unique_ptr.hpp"

#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#endif // !defined(BOOST_LOG_NO_THREADS)

#include <boost/log/detail/header.hpp>

namespace qi = boost::spirit::qi;

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace sinks {

BOOST_LOG_ANONYMOUS_NAMESPACE {

    typedef filesystem::filesystem_error filesystem_error;

    //! A possible Boost.Filesystem extension - renames or moves the file to the target storage
    inline void move_file(
        filesystem::path const& from,
        filesystem::path const& to)
    {
#if defined(BOOST_WINDOWS_API)
        // On Windows MoveFile already does what we need
        filesystem::rename(from, to);
#else
        // On POSIX rename fails if the target points to a different device
        system::error_code ec;
        filesystem::rename(from, to, ec);
        if (ec)
        {
            if (BOOST_LIKELY(ec.value() == system::errc::cross_device_link))
            {
                // Attempt to manually move the file instead
                filesystem::copy_file(from, to);
                filesystem::remove(from);
            }
            else
            {
                BOOST_THROW_EXCEPTION(filesystem_error("failed to move file to another location", from, to, ec));
            }
        }
#endif
    }

    typedef filesystem::path::string_type path_string_type;
    typedef path_string_type::value_type path_char_type;

    //! An auxiliary traits that contain various constants and functions regarding string and character operations
    template< typename CharT >
    struct file_char_traits;

    template< >
    struct file_char_traits< char >
    {
        typedef char char_type;

        static const char_type percent = '%';
        static const char_type number_placeholder = 'N';
        static const char_type day_placeholder = 'd';
        static const char_type month_placeholder = 'm';
        static const char_type year_placeholder = 'y';
        static const char_type full_year_placeholder = 'Y';
        static const char_type frac_sec_placeholder = 'f';
        static const char_type seconds_placeholder = 'S';
        static const char_type minutes_placeholder = 'M';
        static const char_type hours_placeholder = 'H';
        static const char_type space = ' ';
        static const char_type plus = '+';
        static const char_type minus = '-';
        static const char_type zero = '0';
        static const char_type dot = '.';
        static const char_type newline = '\n';

        static bool is_digit(char c)
        {
            using namespace std;
            return (isdigit(c) != 0);
        }
        static std::string default_file_name_pattern() { return "%5N.log"; }
    };

#ifndef BOOST_LOG_BROKEN_STATIC_CONSTANTS_LINKAGE
    const file_char_traits< char >::char_type file_char_traits< char >::percent;
    const file_char_traits< char >::char_type file_char_traits< char >::number_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::day_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::month_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::year_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::full_year_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::frac_sec_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::seconds_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::minutes_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::hours_placeholder;
    const file_char_traits< char >::char_type file_char_traits< char >::space;
    const file_char_traits< char >::char_type file_char_traits< char >::plus;
    const file_char_traits< char >::char_type file_char_traits< char >::minus;
    const file_char_traits< char >::char_type file_char_traits< char >::zero;
    const file_char_traits< char >::char_type file_char_traits< char >::dot;
    const file_char_traits< char >::char_type file_char_traits< char >::newline;
#endif // BOOST_LOG_BROKEN_STATIC_CONSTANTS_LINKAGE

    template< >
    struct file_char_traits< wchar_t >
    {
        typedef wchar_t char_type;

        static const char_type percent = L'%';
        static const char_type number_placeholder = L'N';
        static const char_type day_placeholder = L'd';
        static const char_type month_placeholder = L'm';
        static const char_type year_placeholder = L'y';
        static const char_type full_year_placeholder = L'Y';
        static const char_type frac_sec_placeholder = L'f';
        static const char_type seconds_placeholder = L'S';
        static const char_type minutes_placeholder = L'M';
        static const char_type hours_placeholder = L'H';
        static const char_type space = L' ';
        static const char_type plus = L'+';
        static const char_type minus = L'-';
        static const char_type zero = L'0';
        static const char_type dot = L'.';
        static const char_type newline = L'\n';

        static bool is_digit(wchar_t c)
        {
            using namespace std;
            return (iswdigit(c) != 0);
        }
        static std::wstring default_file_name_pattern() { return L"%5N.log"; }
    };

#ifndef BOOST_LOG_BROKEN_STATIC_CONSTANTS_LINKAGE
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::percent;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::number_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::day_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::month_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::year_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::full_year_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::frac_sec_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::seconds_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::minutes_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::hours_placeholder;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::space;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::plus;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::minus;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::zero;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::dot;
    const file_char_traits< wchar_t >::char_type file_char_traits< wchar_t >::newline;
#endif // BOOST_LOG_BROKEN_STATIC_CONSTANTS_LINKAGE

    //! Date and time formatter
    class date_and_time_formatter
    {
    public:
        typedef path_string_type result_type;

    private:
        typedef date_time::time_facet< posix_time::ptime, path_char_type > time_facet_type;

    private:
        mutable time_facet_type m_Facet;
        mutable std::basic_ostringstream< path_char_type > m_Stream;

    public:
        //! Constructor
        date_and_time_formatter() : m_Facet(1u)
        {
        }
        //! Copy constructor
        date_and_time_formatter(date_and_time_formatter const& that) : m_Facet(1u)
        {
        }
        //! The method formats the current date and time according to the format string str and writes the result into it
        path_string_type operator()(path_string_type const& pattern, unsigned int counter) const
        {
            m_Facet.format(pattern.c_str());
            m_Stream.str(path_string_type());
            // Note: the regular operator<< fails because std::use_facet fails to find the facet in the locale because
            // the facet type in Boost.DateTime has hidden visibility. See this ticket:
            // https://svn.boost.org/trac/boost/ticket/11707
            std::ostreambuf_iterator< path_char_type > sbuf_it(m_Stream);
            m_Facet.put(sbuf_it, m_Stream, m_Stream.fill(), boost::log::attributes::local_time_traits::get_clock());
            if (m_Stream.good())
            {
                return m_Stream.str();
            }
            else
            {
                m_Stream.clear();
                return pattern;
            }
        }

        BOOST_DELETED_FUNCTION(date_and_time_formatter& operator= (date_and_time_formatter const&))
    };

    //! The functor formats the file counter into the file name
    class file_counter_formatter
    {
    public:
        typedef path_string_type result_type;

    private:
        //! The position in the pattern where the file counter placeholder is
        path_string_type::size_type m_FileCounterPosition;
        //! File counter width
        std::streamsize m_Width;
        //! The file counter formatting stream
        mutable std::basic_ostringstream< path_char_type > m_Stream;

    public:
        //! Initializing constructor
        file_counter_formatter(path_string_type::size_type pos, unsigned int width) :
            m_FileCounterPosition(pos),
            m_Width(width)
        {
            typedef file_char_traits< path_char_type > traits_t;
            m_Stream.fill(traits_t::zero);
        }
        //! Copy constructor
        file_counter_formatter(file_counter_formatter const& that) :
            m_FileCounterPosition(that.m_FileCounterPosition),
            m_Width(that.m_Width)
        {
            m_Stream.fill(that.m_Stream.fill());
        }

        //! The function formats the file counter into the file name
        path_string_type operator()(path_string_type const& pattern, unsigned int counter) const
        {
            path_string_type file_name = pattern;

            m_Stream.str(path_string_type());
            m_Stream.width(m_Width);
            m_Stream << counter;
            file_name.insert(m_FileCounterPosition, m_Stream.str());

            return file_name;
        }

        BOOST_DELETED_FUNCTION(file_counter_formatter& operator= (file_counter_formatter const&))
    };

    //! The function returns the pattern as the file name
    class empty_formatter
    {
    public:
        typedef path_string_type result_type;

    private:
        path_string_type m_Pattern;

    public:
        //! Initializing constructor
        explicit empty_formatter(path_string_type const& pattern) : m_Pattern(pattern)
        {
        }
        //! Copy constructor
        empty_formatter(empty_formatter const& that) : m_Pattern(that.m_Pattern)
        {
        }

        //! The function returns the pattern as the file name
        path_string_type const& operator() (unsigned int) const
        {
            return m_Pattern;
        }

        BOOST_DELETED_FUNCTION(empty_formatter& operator= (empty_formatter const&))
    };

    //! The function parses the format placeholder for file counter
    bool parse_counter_placeholder(path_string_type::const_iterator& it, path_string_type::const_iterator end, unsigned int& width)
    {
        typedef qi::extract_uint< unsigned int, 10, 1, -1 > width_extract;
        typedef file_char_traits< path_char_type > traits_t;
        if (it == end)
            return false;

        path_char_type c = *it;
        if (c == traits_t::zero || c == traits_t::space || c == traits_t::plus || c == traits_t::minus)
        {
            // Skip filler and alignment specification
            ++it;
            if (it == end)
                return false;
            c = *it;
        }

        if (traits_t::is_digit(c))
        {
            // Parse width
            if (!width_extract::call(it, end, width))
                return false;
            if (it == end)
                return false;
            c = *it;
        }

        if (c == traits_t::dot)
        {
            // Skip precision
            ++it;
            while (it != end && traits_t::is_digit(*it))
                ++it;
            if (it == end)
                return false;
            c = *it;
        }

        if (c == traits_t::number_placeholder)
        {
            ++it;
            return true;
        }

        return false;
    }

    //! The function matches the file name and the pattern
    bool match_pattern(path_string_type const& file_name, path_string_type const& pattern, unsigned int& file_counter, bool& file_counter_parsed)
    {
        typedef qi::extract_uint< unsigned int, 10, 1, -1 > file_counter_extract;
        typedef file_char_traits< path_char_type > traits_t;

        struct local
        {
            // Verifies that the string contains exactly n digits
            static bool scan_digits(path_string_type::const_iterator& it, path_string_type::const_iterator end, std::ptrdiff_t n)
            {
                for (; n > 0; --n)
                {
                    if (it == end)
                        return false;
                    path_char_type c = *it++;
                    if (!traits_t::is_digit(c))
                        return false;
                }
                return true;
            }
        };

        path_string_type::const_iterator
            f_it = file_name.begin(),
            f_end = file_name.end(),
            p_it = pattern.begin(),
            p_end = pattern.end();
        bool placeholder_expected = false;
        while (f_it != f_end && p_it != p_end)
        {
            path_char_type p_c = *p_it, f_c = *f_it;
            if (!placeholder_expected)
            {
                if (p_c == traits_t::percent)
                {
                    placeholder_expected = true;
                    ++p_it;
                }
                else if (p_c == f_c)
                {
                    ++p_it;
                    ++f_it;
                }
                else
                    return false;
            }
            else
            {
                switch (p_c)
                {
                case traits_t::percent: // An escaped '%'
                    if (p_c == f_c)
                    {
                        ++p_it;
                        ++f_it;
                        break;
                    }
                    else
                        return false;

                case traits_t::seconds_placeholder: // Date/time components with 2-digits width
                case traits_t::minutes_placeholder:
                case traits_t::hours_placeholder:
                case traits_t::day_placeholder:
                case traits_t::month_placeholder:
                case traits_t::year_placeholder:
                    if (!local::scan_digits(f_it, f_end, 2))
                        return false;
                    ++p_it;
                    break;

                case traits_t::full_year_placeholder: // Date/time components with 4-digits width
                    if (!local::scan_digits(f_it, f_end, 4))
                        return false;
                    ++p_it;
                    break;

                case traits_t::frac_sec_placeholder: // Fraction seconds width is configuration-dependent
                    typedef posix_time::time_res_traits posix_resolution_traits;
                    if (!local::scan_digits(f_it, f_end, posix_resolution_traits::num_fractional_digits()))
                    {
                        return false;
                    }
                    ++p_it;
                    break;

                default: // This should be the file counter placeholder or some unsupported placeholder
                    {
                        path_string_type::const_iterator p = p_it;
                        unsigned int width = 0;
                        if (!parse_counter_placeholder(p, p_end, width))
                        {
                            BOOST_THROW_EXCEPTION(std::invalid_argument("Unsupported placeholder used in pattern for file scanning"));
                        }

                        // Find where the file number ends
                        path_string_type::const_iterator f = f_it;
                        if (!local::scan_digits(f, f_end, width))
                            return false;
                        while (f != f_end && traits_t::is_digit(*f))
                            ++f;

                        if (!file_counter_extract::call(f_it, f, file_counter))
                            return false;

                        file_counter_parsed = true;
                        p_it = p;
                    }
                    break;
                }

                placeholder_expected = false;
            }
        }

        if (p_it == p_end)
        {
            if (f_it != f_end)
            {
                // The actual file name may end with an additional counter
                // that is added by the collector in case if file name clash
                return local::scan_digits(f_it, f_end, std::distance(f_it, f_end));
            }
            else
                return true;
        }
        else
            return false;
    }

    //! The function parses file name pattern and splits it into path and filename and creates a function object that will generate the actual filename from the pattern
    void parse_file_name_pattern(filesystem::path const& pattern, filesystem::path& storage_dir, filesystem::path& file_name_pattern, boost::log::aux::light_function< path_string_type (unsigned int) >& file_name_generator)
    {
        // Note: avoid calling Boost.Filesystem functions that involve path::codecvt()
        // https://svn.boost.org/trac/boost/ticket/9119

        typedef file_char_traits< path_char_type > traits_t;

        file_name_pattern = pattern.filename();
        path_string_type name_pattern = file_name_pattern.native();
        storage_dir = filesystem::absolute(pattern.parent_path());

        // Let's try to find the file counter placeholder
        unsigned int placeholder_count = 0;
        unsigned int width = 0;
        bool counter_found = false;
        path_string_type::size_type counter_pos = 0;
        path_string_type::const_iterator end = name_pattern.end();
        path_string_type::const_iterator it = name_pattern.begin();

        do
        {
            it = std::find(it, end, traits_t::percent);
            if (it == end)
                break;
            path_string_type::const_iterator placeholder_begin = it++;
            if (it == end)
                break;
            if (*it == traits_t::percent)
            {
                // An escaped percent detected
                ++it;
                continue;
            }

            ++placeholder_count;

            if (!counter_found)
            {
                path_string_type::const_iterator it2 = it;
                if (parse_counter_placeholder(it2, end, width))
                {
                    // We've found the file counter placeholder in the pattern
                    counter_found = true;
                    counter_pos = placeholder_begin - name_pattern.begin();
                    name_pattern.erase(counter_pos, it2 - placeholder_begin);
                    --placeholder_count;
                    it = name_pattern.begin() + counter_pos;
                    end = name_pattern.end();
                }
            }
        }
        while (it != end);

        // Construct the formatter functor
        if (placeholder_count > 0)
        {
            if (counter_found)
            {
                // Both counter and date/time placeholder in the pattern
                file_name_generator = boost::bind(date_and_time_formatter(),
                    boost::bind(file_counter_formatter(counter_pos, width), name_pattern, boost::placeholders::_1), boost::placeholders::_1);
            }
            else
            {
                // Only date/time placeholders in the pattern
                file_name_generator = boost::bind(date_and_time_formatter(), name_pattern, boost::placeholders::_1);
            }
        }
        else if (counter_found)
        {
            // Only counter placeholder in the pattern
            file_name_generator = boost::bind(file_counter_formatter(counter_pos, width), name_pattern, boost::placeholders::_1);
        }
        else
        {
            // No placeholders detected
            file_name_generator = empty_formatter(name_pattern);
        }
    }


    class file_collector_repository;

    //! Type of the hook used for sequencing file collectors
    typedef intrusive::list_base_hook<
        intrusive::link_mode< intrusive::safe_link >
    > file_collector_hook;

    //! Log file collector implementation
    class file_collector :
        public file::collector,
        public file_collector_hook,
        public enable_shared_from_this< file_collector >
    {
    private:
        //! Information about a single stored file
        struct file_info
        {
            uintmax_t m_Size;
            std::time_t m_TimeStamp;
            filesystem::path m_Path;
        };
        //! A list of the stored files
        typedef std::list< file_info > file_list;
        //! The string type compatible with the universal path type
        typedef filesystem::path::string_type path_string_type;

    private:
        //! A reference to the repository this collector belongs to
        shared_ptr< file_collector_repository > m_pRepository;

#if !defined(BOOST_LOG_NO_THREADS)
        //! Synchronization mutex
        mutex m_Mutex;
#endif // !defined(BOOST_LOG_NO_THREADS)

        //! Total file size upper limit
        uintmax_t m_MaxSize;
        //! Free space lower limit
        uintmax_t m_MinFreeSpace;
        //! File count upper limit
        uintmax_t m_MaxFiles;

        //! The current path at the point when the collector is created
        /*
         * The special member is required to calculate absolute paths with no
         * dependency on the current path for the application, which may change
         */
        const filesystem::path m_BasePath;
        //! Target directory to store files to
        filesystem::path m_StorageDir;

        //! The list of stored files
        file_list m_Files;
        //! Total size of the stored files
        uintmax_t m_TotalSize;

    public:
        //! Constructor
        file_collector(
            shared_ptr< file_collector_repository > const& repo,
            filesystem::path const& target_dir,
            uintmax_t max_size,
            uintmax_t min_free_space,
            uintmax_t max_files);

        //! Destructor
        ~file_collector() BOOST_OVERRIDE;

        //! The function stores the specified file in the storage
        void store_file(filesystem::path const& file_name) BOOST_OVERRIDE;

        //! Scans the target directory for the files that have already been stored
        uintmax_t scan_for_files(
            file::scan_method method, filesystem::path const& pattern, unsigned int* counter) BOOST_OVERRIDE;

        //! The function updates storage restrictions
        void update(uintmax_t max_size, uintmax_t min_free_space, uintmax_t max_files);

        //! The function checks if the directory is governed by this collector
        bool is_governed(filesystem::path const& dir) const
        {
            return filesystem::equivalent(m_StorageDir, dir);
        }

    private:
        //! Makes relative path absolute with respect to the base path
        filesystem::path make_absolute(filesystem::path const& p)
        {
            return filesystem::absolute(p, m_BasePath);
        }
        //! Acquires file name string from the path
        static path_string_type filename_string(filesystem::path const& p)
        {
            return p.filename().string< path_string_type >();
        }
    };


    //! The singleton of the list of file collectors
    class file_collector_repository :
        public log::aux::lazy_singleton< file_collector_repository, shared_ptr< file_collector_repository > >
    {
    private:
        //! Base type
        typedef log::aux::lazy_singleton< file_collector_repository, shared_ptr< file_collector_repository > > base_type;

#if !defined(BOOST_LOG_BROKEN_FRIEND_TEMPLATE_SPECIALIZATIONS)
        friend class log::aux::lazy_singleton< file_collector_repository, shared_ptr< file_collector_repository > >;
#else
        friend class base_type;
#endif

        //! The type of the list of collectors
        typedef intrusive::list<
            file_collector,
            intrusive::base_hook< file_collector_hook >
        > file_collectors;

    private:
#if !defined(BOOST_LOG_NO_THREADS)
        //! Synchronization mutex
        mutex m_Mutex;
#endif // !defined(BOOST_LOG_NO_THREADS)
        //! The list of file collectors
        file_collectors m_Collectors;

    public:
        //! Finds or creates a file collector
        shared_ptr< file::collector > get_collector(
            filesystem::path const& target_dir, uintmax_t max_size, uintmax_t min_free_space, uintmax_t max_files);

        //! Removes the file collector from the list
        void remove_collector(file_collector* p);

    private:
        //! Initializes the singleton instance
        static void init_instance()
        {
            base_type::get_instance() = boost::make_shared< file_collector_repository >();
        }
    };

    //! Constructor
    file_collector::file_collector(
        shared_ptr< file_collector_repository > const& repo,
        filesystem::path const& target_dir,
        uintmax_t max_size,
        uintmax_t min_free_space,
        uintmax_t max_files
    ) :
        m_pRepository(repo),
        m_MaxSize(max_size),
        m_MinFreeSpace(min_free_space),
        m_MaxFiles(max_files),
        m_BasePath(filesystem::current_path()),
        m_TotalSize(0)
    {
        m_StorageDir = make_absolute(target_dir);
        filesystem::create_directories(m_StorageDir);
    }

    //! Destructor
    file_collector::~file_collector()
    {
        m_pRepository->remove_collector(this);
    }

    //! The function stores the specified file in the storage
    void file_collector::store_file(filesystem::path const& src_path)
    {
        // NOTE FOR THE FOLLOWING CODE:
        // Avoid using Boost.Filesystem functions that would call path::codecvt(). store_file() can be called
        // at process termination, and the global codecvt facet can already be destroyed at this point.
        // https://svn.boost.org/trac/boost/ticket/8642

        // Let's construct the new file name
        file_info info;
        info.m_TimeStamp = filesystem::last_write_time(src_path);
        info.m_Size = filesystem::file_size(src_path);

        const filesystem::path file_name_path = src_path.filename();
        path_string_type const& file_name = file_name_path.native();
        info.m_Path = m_StorageDir / file_name_path;

        // Check if the file is already in the target directory
        filesystem::path src_dir = src_path.has_parent_path() ?
                            filesystem::system_complete(src_path.parent_path()) :
                            m_BasePath;
        const bool is_in_target_dir = filesystem::equivalent(src_dir, m_StorageDir);
        if (!is_in_target_dir)
        {
            if (filesystem::exists(info.m_Path))
            {
                // If the file already exists, try to mangle the file name
                // to ensure there's no conflict. I'll need to make this customizable some day.
                file_counter_formatter formatter(file_name.size(), 5);
                unsigned int n = 0;
                while (true)
                {
                    path_string_type alt_file_name = formatter(file_name, n);
                    info.m_Path = m_StorageDir / filesystem::path(alt_file_name);
                    if (!filesystem::exists(info.m_Path))
                        break;

                    if (BOOST_UNLIKELY(n == (std::numeric_limits< unsigned int >::max)()))
                    {
                        BOOST_THROW_EXCEPTION(filesystem_error(
                            "Target file exists and an unused fallback file name could not be found",
                            info.m_Path,
                            system::error_code(system::errc::io_error, system::generic_category())));
                    }

                    ++n;
                }
            }

            // The directory should have been created in constructor, but just in case it got deleted since then...
            filesystem::create_directories(m_StorageDir);
        }

        BOOST_LOG_EXPR_IF_MT(lock_guard< mutex > lock(m_Mutex);)

        file_list::iterator it = m_Files.begin();
        const file_list::iterator end = m_Files.end();
        if (is_in_target_dir)
        {
            // If the sink writes log file into the target dir (is_in_target_dir == true), it is possible that after scanning
            // an old file entry refers to the file that is picked up by the sink for writing. Later on, the sink attempts
            // to store the file in the storage. At best, this would result in duplicate file entries. At worst, if the storage
            // limits trigger a deletion and this file get deleted, we may have an entry that refers to no actual file. In any case,
            // the total size of files in the storage will be incorrect. Here we work around this problem and simply remove
            // the old file entry without removing the file. The entry will be re-added to the list later.
            while (it != end)
            {
                system::error_code ec;
                if (filesystem::equivalent(it->m_Path, info.m_Path, ec))
                {
                    m_TotalSize -= it->m_Size;
                    m_Files.erase(it);
                    break;
                }
                else
                {
                    ++it;
                }
            }

            it = m_Files.begin();
        }

        // Check if an old file should be erased
        uintmax_t free_space = m_MinFreeSpace ? filesystem::space(m_StorageDir).available : static_cast< uintmax_t >(0);
        while (it != end &&
            (m_TotalSize + info.m_Size > m_MaxSize || (m_MinFreeSpace && m_MinFreeSpace > free_space) || m_MaxFiles <= m_Files.size()))
        {
            file_info& old_info = *it;
            system::error_code ec;
            filesystem::file_status status = filesystem::status(old_info.m_Path, ec);

            if (status.type() == filesystem::regular_file)
            {
                try
                {
                    filesystem::remove(old_info.m_Path);
                    // Free space has to be queried as it may not increase equally
                    // to the erased file size on compressed filesystems
                    if (m_MinFreeSpace)
                        free_space = filesystem::space(m_StorageDir).available;
                    m_TotalSize -= old_info.m_Size;
                    it = m_Files.erase(it);
                }
                catch (system::system_error&)
                {
                    // Can't erase the file. Maybe it's locked? Never mind...
                    ++it;
                }
            }
            else
            {
                // If it's not a file or is absent, just remove it from the list
                m_TotalSize -= old_info.m_Size;
                it = m_Files.erase(it);
            }
        }

        if (!is_in_target_dir)
        {
            // Move/rename the file to the target storage
            move_file(src_path, info.m_Path);
        }

        m_Files.push_back(info);
        m_TotalSize += info.m_Size;
    }

    //! Scans the target directory for the files that have already been stored
    uintmax_t file_collector::scan_for_files(
        file::scan_method method, filesystem::path const& pattern, unsigned int* counter)
    {
        uintmax_t file_count = 0;
        if (method != file::no_scan)
        {
            filesystem::path dir = m_StorageDir;
            path_string_type mask;
            if (method == file::scan_matching)
            {
                mask = filename_string(pattern);
                if (pattern.has_parent_path())
                    dir = make_absolute(pattern.parent_path());
            }
            else
            {
                counter = NULL;
            }

            system::error_code ec;
            filesystem::file_status status = filesystem::status(dir, ec);
            if (status.type() == filesystem::directory_file)
            {
                BOOST_LOG_EXPR_IF_MT(lock_guard< mutex > lock(m_Mutex);)

                if (counter)
                    *counter = 0;

                file_list files;
                filesystem::directory_iterator it(dir), end;
                uintmax_t total_size = 0;
                for (; it != end; ++it)
                {
                    filesystem::directory_entry const& dir_entry = *it;
                    file_info info;
                    info.m_Path = dir_entry.path();
                    status = dir_entry.status(ec);
                    if (status.type() == filesystem::regular_file)
                    {
                        // Check that there are no duplicates in the resulting list
                        struct local
                        {
                            static bool equivalent(filesystem::path const& left, file_info const& right)
                            {
                                return filesystem::equivalent(left, right.m_Path);
                            }
                        };
                        if (std::find_if(m_Files.begin(), m_Files.end(),
                            boost::bind(&local::equivalent, boost::cref(info.m_Path), boost::placeholders::_1)) == m_Files.end())
                        {
                            // Check that the file name matches the pattern
                            unsigned int file_number = 0;
                            bool file_number_parsed = false;
                            if (method != file::scan_matching ||
                                match_pattern(filename_string(info.m_Path), mask, file_number, file_number_parsed))
                            {
                                info.m_Size = filesystem::file_size(info.m_Path);
                                total_size += info.m_Size;
                                info.m_TimeStamp = filesystem::last_write_time(info.m_Path);
                                files.push_back(info);
                                ++file_count;

                                // Test that the file_number >= *counter accounting for the integer overflow
                                if (file_number_parsed && counter != NULL && (file_number - *counter) < ((~0u) ^ ((~0u) >> 1)))
                                    *counter = file_number + 1u;
                            }
                        }
                    }
                }

                // Sort files chronologically
                m_Files.splice(m_Files.end(), files);
                m_TotalSize += total_size;
                m_Files.sort(boost::bind(&file_info::m_TimeStamp, boost::placeholders::_1) < boost::bind(&file_info::m_TimeStamp, boost::placeholders::_2));
            }
        }

        return file_count;
    }

    //! The function updates storage restrictions
    void file_collector::update(uintmax_t max_size, uintmax_t min_free_space, uintmax_t max_files)
    {
        BOOST_LOG_EXPR_IF_MT(lock_guard< mutex > lock(m_Mutex);)

        m_MaxSize = (std::min)(m_MaxSize, max_size);
        m_MinFreeSpace = (std::max)(m_MinFreeSpace, min_free_space);
        m_MaxFiles = (std::min)(m_MaxFiles, max_files);
    }


    //! Finds or creates a file collector
    shared_ptr< file::collector > file_collector_repository::get_collector(
        filesystem::path const& target_dir, uintmax_t max_size, uintmax_t min_free_space, uintmax_t max_files)
    {
        BOOST_LOG_EXPR_IF_MT(lock_guard< mutex > lock(m_Mutex);)

        file_collectors::iterator it = std::find_if(m_Collectors.begin(), m_Collectors.end(),
            boost::bind(&file_collector::is_governed, boost::placeholders::_1, boost::cref(target_dir)));
        shared_ptr< file_collector > p;
        if (it != m_Collectors.end()) try
        {
            // This may throw if the collector is being currently destroyed
            p = it->shared_from_this();
            p->update(max_size, min_free_space, max_files);
        }
        catch (bad_weak_ptr&)
        {
        }

        if (!p)
        {
            p = boost::make_shared< file_collector >(
                file_collector_repository::get(), target_dir, max_size, min_free_space, max_files);
            m_Collectors.push_back(*p);
        }

        return p;
    }

    //! Removes the file collector from the list
    void file_collector_repository::remove_collector(file_collector* p)
    {
        BOOST_LOG_EXPR_IF_MT(lock_guard< mutex > lock(m_Mutex);)
        m_Collectors.erase(m_Collectors.iterator_to(*p));
    }

    //! Checks if the time point is valid
    void check_time_point_validity(unsigned char hour, unsigned char minute, unsigned char second)
    {
        if (BOOST_UNLIKELY(hour >= 24))
        {
            std::ostringstream strm;
            strm << "Time point hours value is out of range: " << static_cast< unsigned int >(hour);
            BOOST_THROW_EXCEPTION(std::out_of_range(strm.str()));
        }
        if (BOOST_UNLIKELY(minute >= 60))
        {
            std::ostringstream strm;
            strm << "Time point minutes value is out of range: " << static_cast< unsigned int >(minute);
            BOOST_THROW_EXCEPTION(std::out_of_range(strm.str()));
        }
        if (BOOST_UNLIKELY(second >= 60))
        {
            std::ostringstream strm;
            strm << "Time point seconds value is out of range: " << static_cast< unsigned int >(second);
            BOOST_THROW_EXCEPTION(std::out_of_range(strm.str()));
        }
    }

} // namespace

namespace file {

namespace aux {

    //! Creates and returns a file collector with the specified parameters
    BOOST_LOG_API shared_ptr< collector > make_collector(
        filesystem::path const& target_dir,
        uintmax_t max_size,
        uintmax_t min_free_space,
        uintmax_t max_files)
    {
        return file_collector_repository::get()->get_collector(target_dir, max_size, min_free_space, max_files);
    }

} // namespace aux

//! Creates a rotation time point of every day at the specified time
BOOST_LOG_API rotation_at_time_point::rotation_at_time_point(
    unsigned char hour,
    unsigned char minute,
    unsigned char second
) :
    m_Day(0),
    m_DayKind(not_specified),
    m_Hour(hour),
    m_Minute(minute),
    m_Second(second),
    m_Previous(date_time::not_a_date_time)
{
    check_time_point_validity(hour, minute, second);
}

//! Creates a rotation time point of each specified weekday at the specified time
BOOST_LOG_API rotation_at_time_point::rotation_at_time_point(
    date_time::weekdays wday,
    unsigned char hour,
    unsigned char minute,
    unsigned char second
) :
    m_Day(static_cast< unsigned char >(wday)),
    m_DayKind(weekday),
    m_Hour(hour),
    m_Minute(minute),
    m_Second(second),
    m_Previous(date_time::not_a_date_time)
{
    check_time_point_validity(hour, minute, second);
}

//! Creates a rotation time point of each specified day of month at the specified time
BOOST_LOG_API rotation_at_time_point::rotation_at_time_point(
    gregorian::greg_day mday,
    unsigned char hour,
    unsigned char minute,
    unsigned char second
) :
    m_Day(static_cast< unsigned char >(mday.as_number())),
    m_DayKind(monthday),
    m_Hour(hour),
    m_Minute(minute),
    m_Second(second),
    m_Previous(date_time::not_a_date_time)
{
    check_time_point_validity(hour, minute, second);
}

//! Checks if it's time to rotate the file
BOOST_LOG_API bool rotation_at_time_point::operator()() const
{
    bool result = false;
    posix_time::time_duration rotation_time(
        static_cast< posix_time::time_duration::hour_type >(m_Hour),
        static_cast< posix_time::time_duration::min_type >(m_Minute),
        static_cast< posix_time::time_duration::sec_type >(m_Second));
    posix_time::ptime now = posix_time::second_clock::local_time();

    if (m_Previous.is_special())
    {
        m_Previous = now;
        return false;
    }

    const bool time_of_day_passed = rotation_time.total_seconds() <= m_Previous.time_of_day().total_seconds();
    switch (static_cast< day_kind >(m_DayKind))
    {
    case not_specified:
        {
            // The rotation takes place every day at the specified time
            gregorian::date previous_date = m_Previous.date();
            if (time_of_day_passed)
                previous_date += gregorian::days(1);
            posix_time::ptime next(previous_date, rotation_time);
            result = (now >= next);
        }
        break;

    case weekday:
        {
            // The rotation takes place on the specified week day at the specified time
            gregorian::date previous_date = m_Previous.date(), next_date = previous_date;
            int weekday = m_Day, previous_weekday = static_cast< int >(previous_date.day_of_week().as_number());
            next_date += gregorian::days(weekday - previous_weekday);
            if (weekday < previous_weekday || (weekday == previous_weekday && time_of_day_passed))
            {
                next_date += gregorian::weeks(1);
            }

            posix_time::ptime next(next_date, rotation_time);
            result = (now >= next);
        }
        break;

    case monthday:
        {
            // The rotation takes place on the specified day of month at the specified time
            gregorian::date previous_date = m_Previous.date();
            gregorian::date::day_type monthday = static_cast< gregorian::date::day_type >(m_Day),
                previous_monthday = previous_date.day();
            gregorian::date next_date(previous_date.year(), previous_date.month(), monthday);
            if (monthday < previous_monthday || (monthday == previous_monthday && time_of_day_passed))
            {
                next_date += gregorian::months(1);
            }

            posix_time::ptime next(next_date, rotation_time);
            result = (now >= next);
        }
        break;

    default:
        break;
    }

    if (result)
        m_Previous = now;

    return result;
}

//! Checks if it's time to rotate the file
BOOST_LOG_API bool rotation_at_time_interval::operator()() const
{
    bool result = false;
    posix_time::ptime now = posix_time::second_clock::universal_time();
    if (m_Previous.is_special())
    {
        m_Previous = now;
        return false;
    }

    result = (now - m_Previous) >= m_Interval;

    if (result)
        m_Previous = now;

    return result;
}

} // namespace file

////////////////////////////////////////////////////////////////////////////////
//  File sink backend implementation
////////////////////////////////////////////////////////////////////////////////
//! Sink implementation data
struct text_file_backend::implementation
{
    //! File open mode
    std::ios_base::openmode m_FileOpenMode;

    //! File name pattern
    filesystem::path m_FileNamePattern;
    //! Directory to store files in
    filesystem::path m_StorageDir;
    //! File name generator (according to m_FileNamePattern)
    boost::log::aux::light_function< path_string_type (unsigned int) > m_FileNameGenerator;

    //! Target file name pattern
    filesystem::path m_TargetFileNamePattern;
    //! Target directory to store files in
    filesystem::path m_TargetStorageDir;
    //! Target file name generator (according to m_TargetFileNamePattern)
    boost::log::aux::light_function< path_string_type (unsigned int) > m_TargetFileNameGenerator;

    //! Stored files counter
    unsigned int m_FileCounter;

    //! Current file name
    filesystem::path m_FileName;
    //! File stream
    filesystem::ofstream m_File;
    //! Characters written
    uintmax_t m_CharactersWritten;

    //! File collector functional object
    shared_ptr< file::collector > m_pFileCollector;
    //! File open handler
    open_handler_type m_OpenHandler;
    //! File close handler
    close_handler_type m_CloseHandler;

    //! The maximum temp file size, in characters written to the stream
    uintmax_t m_FileRotationSize;
    //! Time-based rotation predicate
    time_based_rotation_predicate m_TimeBasedRotation;
    //! Indicates whether to append a trailing newline after every log record
    auto_newline_mode m_AutoNewlineMode;
    //! The flag shows if every written record should be flushed
    bool m_AutoFlush;
    //! The flag indicates whether the final rotation should be performed
    bool m_FinalRotationEnabled;

    implementation(uintmax_t rotation_size, auto_newline_mode auto_newline, bool auto_flush, bool enable_final_rotation) :
        m_FileOpenMode(std::ios_base::trunc | std::ios_base::out),
        m_FileCounter(0),
        m_CharactersWritten(0),
        m_FileRotationSize(rotation_size),
        m_AutoNewlineMode(auto_newline),
        m_AutoFlush(auto_flush),
        m_FinalRotationEnabled(enable_final_rotation)
    {
    }
};

//! Constructor. No streams attached to the constructed backend, auto flush feature disabled.
BOOST_LOG_API text_file_backend::text_file_backend()
{
    construct(log::aux::empty_arg_list());
}

//! Destructor
BOOST_LOG_API text_file_backend::~text_file_backend()
{
    try
    {
        // Attempt to put the temporary file into storage
        if (m_pImpl->m_FinalRotationEnabled && m_pImpl->m_File.is_open() && m_pImpl->m_CharactersWritten > 0)
            rotate_file();
    }
    catch (...)
    {
    }

    delete m_pImpl;
}

//! Constructor implementation
BOOST_LOG_API void text_file_backend::construct(
    filesystem::path const& pattern,
    filesystem::path const& target_file_name,
    std::ios_base::openmode mode,
    uintmax_t rotation_size,
    time_based_rotation_predicate const& time_based_rotation,
    auto_newline_mode auto_newline,
    bool auto_flush,
    bool enable_final_rotation)
{
    m_pImpl = new implementation(rotation_size, auto_newline, auto_flush, enable_final_rotation);
    set_file_name_pattern_internal(pattern);
    set_target_file_name_pattern_internal(target_file_name);
    set_time_based_rotation(time_based_rotation);
    set_open_mode(mode);
}

//! The method sets maximum file size.
BOOST_LOG_API void text_file_backend::set_rotation_size(uintmax_t size)
{
    m_pImpl->m_FileRotationSize = size;
}

//! The method sets the maximum time interval between file rotations.
BOOST_LOG_API void text_file_backend::set_time_based_rotation(time_based_rotation_predicate const& predicate)
{
    m_pImpl->m_TimeBasedRotation = predicate;
}

//! The method allows to enable or disable log file rotation on sink destruction.
BOOST_LOG_API void text_file_backend::enable_final_rotation(bool enable)
{
    m_pImpl->m_FinalRotationEnabled = enable;
}

//! Sets the flag to automatically flush write buffers of the file being written after each log record.
BOOST_LOG_API void text_file_backend::auto_flush(bool enable)
{
    m_pImpl->m_AutoFlush = enable;
}

//! Selects whether a trailing newline should be automatically inserted after every log record.
BOOST_LOG_API void text_file_backend::set_auto_newline_mode(auto_newline_mode mode)
{
    m_pImpl->m_AutoNewlineMode = mode;
}

//! The method writes the message to the sink
BOOST_LOG_API void text_file_backend::consume(record_view const& rec, string_type const& formatted_message)
{
    typedef file_char_traits< string_type::value_type > traits_t;

    filesystem::path prev_file_name;
    bool use_prev_file_name = false;
    if (BOOST_UNLIKELY(!m_pImpl->m_File.good()))
    {
        // The file stream is not operational. One possible reason is that there is no more free space
        // on the file system. In this case it is possible that this log record will fail to be written as well,
        // leaving the newly created file empty. Eventually this results in lots of empty log files.
        // We should take precautions to avoid this. https://svn.boost.org/trac/boost/ticket/11016
        prev_file_name = m_pImpl->m_FileName;
        close_file();

        system::error_code ec;
        uintmax_t size = filesystem::file_size(prev_file_name, ec);
        if (!!ec || size == 0)
        {
            // To reuse the empty file avoid re-generating the new file name later
            use_prev_file_name = true;
        }
        else if (!!m_pImpl->m_pFileCollector)
        {
            // Complete file rotation
            m_pImpl->m_pFileCollector->store_file(prev_file_name);
        }
    }
    else if
    (
        m_pImpl->m_File.is_open() &&
        (
            m_pImpl->m_CharactersWritten + formatted_message.size() >= m_pImpl->m_FileRotationSize ||
            (!m_pImpl->m_TimeBasedRotation.empty() && m_pImpl->m_TimeBasedRotation())
        )
    )
    {
        rotate_file();
    }

    if (!m_pImpl->m_File.is_open())
    {
        filesystem::path new_file_name;
        if (!use_prev_file_name)
            new_file_name = m_pImpl->m_StorageDir / m_pImpl->m_FileNameGenerator(m_pImpl->m_FileCounter++);
        else
            prev_file_name.swap(new_file_name);

        filesystem::create_directories(new_file_name.parent_path());

        m_pImpl->m_File.open(new_file_name, m_pImpl->m_FileOpenMode);
        if (BOOST_UNLIKELY(!m_pImpl->m_File.is_open()))
        {
            BOOST_THROW_EXCEPTION(filesystem_error(
                "Failed to open file for writing",
                new_file_name,
                system::error_code(system::errc::io_error, system::generic_category())));
        }
        m_pImpl->m_FileName.swap(new_file_name);

        if (!m_pImpl->m_OpenHandler.empty())
            m_pImpl->m_OpenHandler(m_pImpl->m_File);

        m_pImpl->m_CharactersWritten = static_cast< std::streamoff >(m_pImpl->m_File.tellp());
    }

    m_pImpl->m_File.write(formatted_message.data(), static_cast< std::streamsize >(formatted_message.size()));
    m_pImpl->m_CharactersWritten += formatted_message.size();

    if (m_pImpl->m_AutoNewlineMode != disabled_auto_newline)
    {
        if (m_pImpl->m_AutoNewlineMode == always_insert || formatted_message.empty() || *formatted_message.rbegin() != traits_t::newline)
        {
            m_pImpl->m_File.put(traits_t::newline);
            ++m_pImpl->m_CharactersWritten;
        }
    }

    if (m_pImpl->m_AutoFlush)
        m_pImpl->m_File.flush();
}

//! The method flushes the currently open log file
BOOST_LOG_API void text_file_backend::flush()
{
    if (m_pImpl->m_File.is_open())
        m_pImpl->m_File.flush();
}

//! The method sets file name pattern
BOOST_LOG_API void text_file_backend::set_file_name_pattern_internal(filesystem::path const& pattern)
{
    typedef file_char_traits< path_char_type > traits_t;

    parse_file_name_pattern
    (
        !pattern.empty() ? pattern : filesystem::path(traits_t::default_file_name_pattern()),
        m_pImpl->m_StorageDir,
        m_pImpl->m_FileNamePattern,
        m_pImpl->m_FileNameGenerator
    );
}

//! The method sets target file name pattern
BOOST_LOG_API void text_file_backend::set_target_file_name_pattern_internal(filesystem::path const& pattern)
{
    if (!pattern.empty())
    {
        parse_file_name_pattern(pattern, m_pImpl->m_TargetStorageDir, m_pImpl->m_TargetFileNamePattern, m_pImpl->m_TargetFileNameGenerator);
    }
    else
    {
        m_pImpl->m_TargetStorageDir.clear();
        m_pImpl->m_TargetFileNamePattern.clear();
        m_pImpl->m_TargetFileNameGenerator.clear();
    }
}

//! Closes the currently open file
void text_file_backend::close_file()
{
    if (m_pImpl->m_File.is_open())
    {
        if (!m_pImpl->m_CloseHandler.empty())
        {
            // Rationale: We should call the close handler even if the stream is !good() because
            // writing the footer may not be the only thing the handler does. However, there is
            // a chance that the file had become writable since the last failure (e.g. there was
            // no space left to write the last record, but it got freed since then), so if the handler
            // attempts to write a footer it may succeed now. For this reason we clear the stream state
            // and let the handler have a try.
            m_pImpl->m_File.clear();
            m_pImpl->m_CloseHandler(m_pImpl->m_File);
        }

        m_pImpl->m_File.close();
    }

    m_pImpl->m_File.clear();
    m_pImpl->m_CharactersWritten = 0;
    m_pImpl->m_FileName.clear();
}

//! The method rotates the file
BOOST_LOG_API void text_file_backend::rotate_file()
{
    filesystem::path prev_file_name = m_pImpl->m_FileName;
    close_file();

    // Check if the file has been created in the first place
    system::error_code ec;
    filesystem::file_status status = filesystem::status(prev_file_name, ec);
    if (status.type() == filesystem::regular_file)
    {
        if (!!m_pImpl->m_TargetFileNameGenerator)
        {
            // File counter was incremented when the file was opened, we have to use the same counter value we used to generate the original filename
            filesystem::path new_file_name = m_pImpl->m_TargetStorageDir / m_pImpl->m_TargetFileNameGenerator(m_pImpl->m_FileCounter - 1u);

            if (new_file_name != prev_file_name)
            {
                filesystem::create_directories(new_file_name.parent_path());
                move_file(prev_file_name, new_file_name);

                prev_file_name.swap(new_file_name);
            }
        }

        if (!!m_pImpl->m_pFileCollector)
            m_pImpl->m_pFileCollector->store_file(prev_file_name);
    }
}

//! The method sets the file open mode
BOOST_LOG_API void text_file_backend::set_open_mode(std::ios_base::openmode mode)
{
    mode |= std::ios_base::out;
    mode &= ~std::ios_base::in;
    if ((mode & (std::ios_base::trunc | std::ios_base::app)) == 0)
        mode |= std::ios_base::trunc;
    m_pImpl->m_FileOpenMode = mode;
}

//! The method sets file collector
BOOST_LOG_API void text_file_backend::set_file_collector(shared_ptr< file::collector > const& collector)
{
    m_pImpl->m_pFileCollector = collector;
}

//! The method sets file open handler
BOOST_LOG_API void text_file_backend::set_open_handler(open_handler_type const& handler)
{
    m_pImpl->m_OpenHandler = handler;
}

//! The method sets file close handler
BOOST_LOG_API void text_file_backend::set_close_handler(close_handler_type const& handler)
{
    m_pImpl->m_CloseHandler = handler;
}

//! The method returns name of the currently open log file. If no file is open, returns an empty path.
BOOST_LOG_API filesystem::path text_file_backend::get_current_file_name() const
{
    return m_pImpl->m_FileName;
}

//! Performs scanning of the target directory for log files
BOOST_LOG_API uintmax_t text_file_backend::scan_for_files(file::scan_method method, bool update_counter)
{
    if (BOOST_LIKELY(!!m_pImpl->m_pFileCollector))
    {
        unsigned int* counter = update_counter ? &m_pImpl->m_FileCounter : static_cast< unsigned int* >(NULL);
        return m_pImpl->m_pFileCollector->scan_for_files
        (
            method,
            m_pImpl->m_TargetFileNamePattern.empty() ? m_pImpl->m_FileNamePattern : m_pImpl->m_TargetFileNamePattern,
            counter
        );
    }
    else
    {
        BOOST_LOG_THROW_DESCR(setup_error, "File collector is not set");
    }
}

} // namespace sinks

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
