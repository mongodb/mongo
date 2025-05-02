//  boost/filesystem/directory.hpp  ---------------------------------------------------//

//  Copyright Beman Dawes 2002-2009
//  Copyright Jan Langer 2002
//  Copyright Dietmar Kuehl 2001
//  Copyright Vladimir Prus 2002
//  Copyright Andrey Semashev 2019, 2022

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_DIRECTORY_HPP
#define BOOST_FILESYSTEM_DIRECTORY_HPP

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/detail/path_traits.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <boost/assert.hpp>
#include <boost/detail/bitmask.hpp>
#include <boost/system/error_code.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/iterator_categories.hpp>

#include <boost/filesystem/detail/header.hpp> // must be the last #include

//--------------------------------------------------------------------------------------//

namespace boost {
namespace filesystem {

enum class directory_options : unsigned int
{
    none = 0u,
    skip_permission_denied = 1u,         // if a directory cannot be opened because of insufficient permissions, pretend that the directory is empty
    follow_directory_symlink = 1u << 1u, // recursive_directory_iterator: follow directory symlinks
    skip_dangling_symlinks = 1u << 2u,   // non-standard extension for recursive_directory_iterator: don't follow dangling directory symlinks,
    pop_on_error = 1u << 3u,             // non-standard extension for recursive_directory_iterator: instead of producing an end iterator on errors,
                                         // repeatedly invoke pop() until it succeeds or the iterator becomes equal to end iterator
    _detail_no_follow = 1u << 4u,        // internal use only
    _detail_no_push = 1u << 5u           // internal use only
};

BOOST_BITMASK(directory_options)

class directory_iterator;
class recursive_directory_iterator;

namespace detail {

struct directory_iterator_params;

BOOST_FILESYSTEM_DECL void directory_iterator_construct(directory_iterator& it, path const& p, directory_options opts, directory_iterator_params* params, system::error_code* ec);
BOOST_FILESYSTEM_DECL void directory_iterator_increment(directory_iterator& it, system::error_code* ec);

struct recur_dir_itr_imp;

BOOST_FILESYSTEM_DECL void recursive_directory_iterator_construct(recursive_directory_iterator& it, path const& dir_path, directory_options opts, system::error_code* ec);
BOOST_FILESYSTEM_DECL void recursive_directory_iterator_increment(recursive_directory_iterator& it, system::error_code* ec);
BOOST_FILESYSTEM_DECL void recursive_directory_iterator_pop(recursive_directory_iterator& it, system::error_code* ec);

} // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                 directory_entry                                      //
//                                                                                      //
//--------------------------------------------------------------------------------------//

//  GCC has a problem with a member function named path within a namespace or
//  sub-namespace that also has a class named path. The workaround is to always
//  fully qualify the name path when it refers to the class name.

class directory_entry
{
    friend BOOST_FILESYSTEM_DECL void detail::directory_iterator_construct(directory_iterator& it, path const& p, directory_options opts, detail::directory_iterator_params* params, system::error_code* ec);
    friend BOOST_FILESYSTEM_DECL void detail::directory_iterator_increment(directory_iterator& it, system::error_code* ec);

    friend BOOST_FILESYSTEM_DECL void detail::recursive_directory_iterator_increment(recursive_directory_iterator& it, system::error_code* ec);

public:
    typedef boost::filesystem::path::value_type value_type; // enables class path ctor taking directory_entry

    directory_entry() noexcept {}

    explicit directory_entry(boost::filesystem::path const& p);

#if BOOST_FILESYSTEM_VERSION >= 4
    directory_entry(boost::filesystem::path const& p, system::error_code& ec) :
        m_path(p)
    {
        refresh_impl(&ec);
        if (ec)
            m_path.clear();
    }
#else
    directory_entry(boost::filesystem::path const& p, file_status st, file_status symlink_st = file_status()) :
        m_path(p), m_status(st), m_symlink_status(symlink_st)
    {
    }
#endif

    directory_entry(directory_entry const& rhs) :
        m_path(rhs.m_path), m_status(rhs.m_status), m_symlink_status(rhs.m_symlink_status)
    {
    }

    directory_entry& operator=(directory_entry const& rhs)
    {
        m_path = rhs.m_path;
        m_status = rhs.m_status;
        m_symlink_status = rhs.m_symlink_status;
        return *this;
    }

    directory_entry(directory_entry&& rhs) noexcept :
        m_path(static_cast< boost::filesystem::path&& >(rhs.m_path)),
        m_status(static_cast< file_status&& >(rhs.m_status)),
        m_symlink_status(static_cast< file_status&& >(rhs.m_symlink_status))
    {
    }

    directory_entry& operator=(directory_entry&& rhs) noexcept
    {
        m_path = static_cast< boost::filesystem::path&& >(rhs.m_path);
        m_status = static_cast< file_status&& >(rhs.m_status);
        m_symlink_status = static_cast< file_status&& >(rhs.m_symlink_status);
        return *this;
    }

    void assign(boost::filesystem::path&& p);

#if BOOST_FILESYSTEM_VERSION >= 4
    void assign(boost::filesystem::path&& p, system::error_code& ec)
    {
        m_path = static_cast< boost::filesystem::path&& >(p);
        refresh_impl(&ec);
    }
#else
    void assign(boost::filesystem::path&& p, file_status st, file_status symlink_st = file_status())
    {
        assign_with_status(static_cast< boost::filesystem::path&& >(p), st, symlink_st);
    }
#endif

    void assign(boost::filesystem::path const& p);

#if BOOST_FILESYSTEM_VERSION >= 4
    void assign(boost::filesystem::path const& p, system::error_code& ec)
    {
        m_path = p;
        refresh_impl(&ec);
    }
#else
    void assign(boost::filesystem::path const& p, file_status st, file_status symlink_st = file_status())
    {
        assign_with_status(p, st, symlink_st);
    }
#endif

    void replace_filename(boost::filesystem::path const& p);

#if BOOST_FILESYSTEM_VERSION >= 4
    void replace_filename(boost::filesystem::path const& p, system::error_code& ec)
    {
        m_path.replace_filename(p);
        refresh_impl(&ec);
    }
#else
    void replace_filename(boost::filesystem::path const& p, file_status st, file_status symlink_st = file_status())
    {
        replace_filename_with_status(p, st, symlink_st);
    }

    BOOST_FILESYSTEM_DETAIL_DEPRECATED("Use directory_entry::replace_filename() instead")
    void replace_leaf(boost::filesystem::path const& p, file_status st, file_status symlink_st)
    {
        replace_filename_with_status(p, st, symlink_st);
    }
#endif

    boost::filesystem::path const& path() const noexcept { return m_path; }
    operator boost::filesystem::path const&() const noexcept { return m_path; }

    void refresh() { refresh_impl(); }
    void refresh(system::error_code& ec) noexcept { refresh_impl(&ec); }

    file_status status() const
    {
        if (!filesystem::status_known(m_status))
            refresh_impl();
        return m_status;
    }

    file_status status(system::error_code& ec) const noexcept
    {
        ec.clear();

        if (!filesystem::status_known(m_status))
            refresh_impl(&ec);
        return m_status;
    }

    file_status symlink_status() const
    {
        if (!filesystem::status_known(m_symlink_status))
            refresh_impl();
        return m_symlink_status;
    }

    file_status symlink_status(system::error_code& ec) const noexcept
    {
        ec.clear();

        if (!filesystem::status_known(m_symlink_status))
            refresh_impl(&ec);
        return m_symlink_status;
    }

    filesystem::file_type file_type() const
    {
        if (!filesystem::type_present(m_status))
            refresh_impl();
        return m_status.type();
    }

    filesystem::file_type file_type(system::error_code& ec) const noexcept
    {
        ec.clear();

        if (!filesystem::type_present(m_status))
            refresh_impl(&ec);
        return m_status.type();
    }

    filesystem::file_type symlink_file_type() const
    {
        if (!filesystem::type_present(m_symlink_status))
            refresh_impl();
        return m_symlink_status.type();
    }

    filesystem::file_type symlink_file_type(system::error_code& ec) const noexcept
    {
        ec.clear();

        if (!filesystem::type_present(m_symlink_status))
            refresh_impl(&ec);
        return m_symlink_status.type();
    }

    bool exists() const
    {
        filesystem::file_type ft = this->file_type();
        return ft != filesystem::status_error && ft != filesystem::file_not_found;
    }

    bool exists(system::error_code& ec) const noexcept
    {
        filesystem::file_type ft = this->file_type(ec);
        return ft != filesystem::status_error && ft != filesystem::file_not_found;
    }

    bool is_regular_file() const
    {
        return this->file_type() == filesystem::regular_file;
    }

    bool is_regular_file(system::error_code& ec) const noexcept
    {
        return this->file_type(ec) == filesystem::regular_file;
    }

    bool is_directory() const
    {
        return this->file_type() == filesystem::directory_file;
    }

    bool is_directory(system::error_code& ec) const noexcept
    {
        return this->file_type(ec) == filesystem::directory_file;
    }

    bool is_symlink() const
    {
        return this->symlink_file_type() == filesystem::symlink_file;
    }

    bool is_symlink(system::error_code& ec) const noexcept
    {
        return this->symlink_file_type(ec) == filesystem::symlink_file;
    }

    bool is_block_file() const
    {
        return this->file_type() == filesystem::block_file;
    }

    bool is_block_file(system::error_code& ec) const noexcept
    {
        return this->file_type(ec) == filesystem::block_file;
    }

    bool is_character_file() const
    {
        return this->file_type() == filesystem::character_file;
    }

    bool is_character_file(system::error_code& ec) const noexcept
    {
        return this->file_type(ec) == filesystem::character_file;
    }

    bool is_fifo() const
    {
        return this->file_type() == filesystem::fifo_file;
    }

    bool is_fifo(system::error_code& ec) const noexcept
    {
        return this->file_type(ec) == filesystem::fifo_file;
    }

    bool is_socket() const
    {
        return this->file_type() == filesystem::socket_file;
    }

    bool is_socket(system::error_code& ec) const noexcept
    {
        return this->file_type(ec) == filesystem::socket_file;
    }

    bool is_reparse_file() const
    {
        return this->symlink_file_type() == filesystem::reparse_file;
    }

    bool is_reparse_file(system::error_code& ec) const noexcept
    {
        return this->symlink_file_type(ec) == filesystem::reparse_file;
    }

    bool is_other() const
    {
        filesystem::file_type ft = this->file_type();
        return ft != filesystem::status_error && ft != filesystem::file_not_found &&
            ft != filesystem::regular_file && ft != filesystem::directory_file;
    }

    bool is_other(system::error_code& ec) const noexcept
    {
        filesystem::file_type ft = this->file_type(ec);
        return ft != filesystem::status_error && ft != filesystem::file_not_found &&
            ft != filesystem::regular_file && ft != filesystem::directory_file;
    }

    bool operator==(directory_entry const& rhs) const { return m_path == rhs.m_path; }
    bool operator!=(directory_entry const& rhs) const { return m_path != rhs.m_path; }
    bool operator<(directory_entry const& rhs) const { return m_path < rhs.m_path; }
    bool operator<=(directory_entry const& rhs) const { return m_path <= rhs.m_path; }
    bool operator>(directory_entry const& rhs) const { return m_path > rhs.m_path; }
    bool operator>=(directory_entry const& rhs) const { return m_path >= rhs.m_path; }

private:
    BOOST_FILESYSTEM_DECL void refresh_impl(system::error_code* ec = nullptr) const;

    void assign_with_status(boost::filesystem::path&& p, file_status st, file_status symlink_st)
    {
        m_path = static_cast< boost::filesystem::path&& >(p);
        m_status = static_cast< file_status&& >(st);
        m_symlink_status = static_cast< file_status&& >(symlink_st);
    }

    void assign_with_status(boost::filesystem::path const& p, file_status st, file_status symlink_st)
    {
        m_path = p;
        m_status = static_cast< file_status&& >(st);
        m_symlink_status = static_cast< file_status&& >(symlink_st);
    }

    void replace_filename_with_status(boost::filesystem::path const& p, file_status st, file_status symlink_st)
    {
        m_path.replace_filename(p);
        m_status = static_cast< file_status&& >(st);
        m_symlink_status = static_cast< file_status&& >(symlink_st);
    }

private:
    boost::filesystem::path m_path;
    mutable file_status m_status;         // stat()-like
    mutable file_status m_symlink_status; // lstat()-like
};

#if !defined(BOOST_FILESYSTEM_SOURCE)

inline directory_entry::directory_entry(boost::filesystem::path const& p) :
    m_path(p)
{
#if BOOST_FILESYSTEM_VERSION >= 4
    refresh_impl();
#endif
}

inline void directory_entry::assign(boost::filesystem::path&& p)
{
    m_path = static_cast< boost::filesystem::path&& >(p);
#if BOOST_FILESYSTEM_VERSION >= 4
    refresh_impl();
#else
    m_status = file_status();
    m_symlink_status = file_status();
#endif
}

inline void directory_entry::assign(boost::filesystem::path const& p)
{
    m_path = p;
#if BOOST_FILESYSTEM_VERSION >= 4
    refresh_impl();
#else
    m_status = file_status();
    m_symlink_status = file_status();
#endif
}

inline void directory_entry::replace_filename(boost::filesystem::path const& p)
{
    m_path.replace_filename(p);
#if BOOST_FILESYSTEM_VERSION >= 4
    refresh_impl();
#else
    m_status = file_status();
    m_symlink_status = file_status();
#endif
}

#endif // !defined(BOOST_FILESYSTEM_SOURCE)

namespace detail {
namespace path_traits {

// Dispatch function for integration with path class
template< typename Callback >
BOOST_FORCEINLINE typename Callback::result_type dispatch(directory_entry const& de, Callback cb, const codecvt_type* cvt, directory_entry_tag)
{
    boost::filesystem::path::string_type const& source = de.path().native();
    return cb(source.data(), source.data() + source.size(), cvt);
}

} // namespace path_traits
} // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            directory_entry overloads                                 //
//                                                                                      //
//--------------------------------------------------------------------------------------//

//  Without these functions, calling (for example) 'is_directory' with a 'directory_entry' results in:
//  - a conversion to 'path' using 'operator boost::filesystem::path const&()',
//  - then a call to 'is_directory(path const& p)' which recomputes the status with 'detail::status(p)'.
//
//  These functions avoid a costly recomputation of the status if one calls 'is_directory(e)' instead of 'is_directory(e.status())'

inline file_status status(directory_entry const& e)
{
    return e.status();
}

inline file_status status(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.status(ec);
}

inline file_status symlink_status(directory_entry const& e)
{
    return e.symlink_status();
}

inline file_status symlink_status(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.symlink_status(ec);
}

inline bool type_present(directory_entry const& e)
{
    return e.file_type() != filesystem::status_error;
}

inline bool type_present(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.file_type(ec) != filesystem::status_error;
}

inline bool status_known(directory_entry const& e)
{
    return filesystem::status_known(e.status());
}

inline bool status_known(directory_entry const& e, system::error_code& ec) noexcept
{
    return filesystem::status_known(e.status(ec));
}

inline bool exists(directory_entry const& e)
{
    return e.exists();
}

inline bool exists(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.exists(ec);
}

inline bool is_regular_file(directory_entry const& e)
{
    return e.is_regular_file();
}

inline bool is_regular_file(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_regular_file(ec);
}

inline bool is_directory(directory_entry const& e)
{
    return e.is_directory();
}

inline bool is_directory(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_directory(ec);
}

inline bool is_symlink(directory_entry const& e)
{
    return e.is_symlink();
}

inline bool is_symlink(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_symlink(ec);
}

inline bool is_block_file(directory_entry const& e)
{
    return e.is_block_file();
}

inline bool is_block_file(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_block_file(ec);
}

inline bool is_character_file(directory_entry const& e)
{
    return e.is_character_file();
}

inline bool is_character_file(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_character_file(ec);
}

inline bool is_fifo(directory_entry const& e)
{
    return e.is_fifo();
}

inline bool is_fifo(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_fifo(ec);
}

inline bool is_socket(directory_entry const& e)
{
    return e.is_socket();
}

inline bool is_socket(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_socket(ec);
}

inline bool is_reparse_file(directory_entry const& e)
{
    return e.is_reparse_file();
}

inline bool is_reparse_file(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_reparse_file(ec);
}

inline bool is_other(directory_entry const& e)
{
    return e.is_other();
}

inline bool is_other(directory_entry const& e, system::error_code& ec) noexcept
{
    return e.is_other(ec);
}

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            directory_iterator helpers                                //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace detail {

struct dir_itr_imp :
    public boost::intrusive_ref_counter< dir_itr_imp >
{
#ifdef BOOST_WINDOWS_API
    bool close_handle;
    unsigned char extra_data_format;
    std::size_t current_offset;
#endif
    directory_entry dir_entry;
    void* handle;

    dir_itr_imp() noexcept :
#ifdef BOOST_WINDOWS_API
        close_handle(false),
        extra_data_format(0u),
        current_offset(0u),
#endif
        handle(nullptr)
    {
    }
    BOOST_FILESYSTEM_DECL ~dir_itr_imp() noexcept;

    BOOST_FILESYSTEM_DECL static void* operator new(std::size_t class_size, std::size_t extra_size) noexcept;
    BOOST_FILESYSTEM_DECL static void operator delete(void* p, std::size_t extra_size) noexcept;
    BOOST_FILESYSTEM_DECL static void operator delete(void* p) noexcept;
};

} // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                directory_iterator                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

class directory_iterator :
    public boost::iterator_facade<
        directory_iterator,
        directory_entry,
        boost::single_pass_traversal_tag
    >
{
    friend class boost::iterator_core_access;

    friend BOOST_FILESYSTEM_DECL void detail::directory_iterator_construct(directory_iterator& it, path const& p, directory_options opts, detail::directory_iterator_params* params, system::error_code* ec);
    friend BOOST_FILESYSTEM_DECL void detail::directory_iterator_increment(directory_iterator& it, system::error_code* ec);

    friend BOOST_FILESYSTEM_DECL void detail::recursive_directory_iterator_increment(recursive_directory_iterator& it, system::error_code* ec);

public:
    directory_iterator() noexcept {} // creates the "end" iterator

    // iterator_facade derived classes don't seem to like implementations in
    // separate translation unit dll's, so forward to detail functions
    explicit directory_iterator(path const& p, directory_options opts = directory_options::none)
    {
        detail::directory_iterator_construct(*this, p, opts, nullptr, nullptr);
    }

    directory_iterator(path const& p, system::error_code& ec) noexcept
    {
        detail::directory_iterator_construct(*this, p, directory_options::none, nullptr, &ec);
    }

    directory_iterator(path const& p, directory_options opts, system::error_code& ec) noexcept
    {
        detail::directory_iterator_construct(*this, p, opts, nullptr, &ec);
    }

    directory_iterator(directory_iterator const&) = default;
    directory_iterator& operator=(directory_iterator const&) = default;

    directory_iterator(directory_iterator&& that) noexcept :
        m_imp(static_cast< boost::intrusive_ptr< detail::dir_itr_imp >&& >(that.m_imp))
    {
    }

    directory_iterator& operator=(directory_iterator&& that) noexcept
    {
        m_imp = static_cast< boost::intrusive_ptr< detail::dir_itr_imp >&& >(that.m_imp);
        return *this;
    }

    directory_iterator& increment(system::error_code& ec) noexcept
    {
        detail::directory_iterator_increment(*this, &ec);
        return *this;
    }

private:
    boost::iterator_facade<
        directory_iterator,
        directory_entry,
        boost::single_pass_traversal_tag
    >::reference dereference() const
    {
        BOOST_ASSERT_MSG(!is_end(), "attempt to dereference end directory iterator");
        return m_imp->dir_entry;
    }

    void increment() { detail::directory_iterator_increment(*this, nullptr); }

    bool equal(directory_iterator const& rhs) const noexcept
    {
        return m_imp == rhs.m_imp || (is_end() && rhs.is_end());
    }

    bool is_end() const noexcept
    {
        // Note: The check for handle is needed because the iterator can be copied and the copy
        // can be incremented to end while the original iterator still refers to the same dir_itr_imp.
        return !m_imp || !m_imp->handle;
    }

private:
    // intrusive_ptr provides the shallow-copy semantics required for single pass iterators
    // (i.e. InputIterators). The end iterator is indicated by is_end().
    boost::intrusive_ptr< detail::dir_itr_imp > m_imp;
};

//  enable directory_iterator C++11 range-based for statement use  --------------------//

// begin() and end() are only used by a range-based for statement in the context of
// auto - thus the top-level const is stripped - so returning const is harmless and
// emphasizes begin() is just a pass through.
inline directory_iterator const& begin(directory_iterator const& iter) noexcept
{
    return iter;
}

inline directory_iterator end(directory_iterator const&) noexcept
{
    return directory_iterator();
}

// enable C++14 generic accessors for range const iterators
inline directory_iterator const& cbegin(directory_iterator const& iter) noexcept
{
    return iter;
}

inline directory_iterator cend(directory_iterator const&) noexcept
{
    return directory_iterator();
}

//  enable directory_iterator BOOST_FOREACH  -----------------------------------------//

inline directory_iterator& range_begin(directory_iterator& iter) noexcept
{
    return iter;
}

inline directory_iterator range_begin(directory_iterator const& iter) noexcept
{
    return iter;
}

inline directory_iterator range_end(directory_iterator&) noexcept
{
    return directory_iterator();
}

inline directory_iterator range_end(directory_iterator const&) noexcept
{
    return directory_iterator();
}

} // namespace filesystem

//  namespace boost template specializations
template< typename C, typename Enabler >
struct range_mutable_iterator;

template<>
struct range_mutable_iterator< boost::filesystem::directory_iterator, void >
{
    typedef boost::filesystem::directory_iterator type;
};

template< typename C, typename Enabler >
struct range_const_iterator;

template<>
struct range_const_iterator< boost::filesystem::directory_iterator, void >
{
    typedef boost::filesystem::directory_iterator type;
};

namespace filesystem {

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                      recursive_directory_iterator helpers                            //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace detail {

struct recur_dir_itr_imp :
    public boost::intrusive_ref_counter< recur_dir_itr_imp >
{
    typedef directory_iterator element_type;
    std::vector< element_type > m_stack;
    directory_options m_options;

    explicit recur_dir_itr_imp(directory_options opts) noexcept : m_options(opts) {}
};

} // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                           recursive_directory_iterator                               //
//                                                                                      //
//--------------------------------------------------------------------------------------//

class recursive_directory_iterator :
    public boost::iterator_facade<
        recursive_directory_iterator,
        directory_entry,
        boost::single_pass_traversal_tag
    >
{
    friend class boost::iterator_core_access;

    friend BOOST_FILESYSTEM_DECL void detail::recursive_directory_iterator_construct(recursive_directory_iterator& it, path const& dir_path, directory_options opts, system::error_code* ec);
    friend BOOST_FILESYSTEM_DECL void detail::recursive_directory_iterator_increment(recursive_directory_iterator& it, system::error_code* ec);
    friend BOOST_FILESYSTEM_DECL void detail::recursive_directory_iterator_pop(recursive_directory_iterator& it, system::error_code* ec);

public:
    recursive_directory_iterator() noexcept {} // creates the "end" iterator

    explicit recursive_directory_iterator(path const& dir_path)
    {
        detail::recursive_directory_iterator_construct(*this, dir_path, directory_options::none, nullptr);
    }

    recursive_directory_iterator(path const& dir_path, system::error_code& ec)
    {
        detail::recursive_directory_iterator_construct(*this, dir_path, directory_options::none, &ec);
    }

    recursive_directory_iterator(path const& dir_path, directory_options opts)
    {
        detail::recursive_directory_iterator_construct(*this, dir_path, opts, nullptr);
    }

    recursive_directory_iterator(path const& dir_path, directory_options opts, system::error_code& ec)
    {
        detail::recursive_directory_iterator_construct(*this, dir_path, opts, &ec);
    }

    recursive_directory_iterator(recursive_directory_iterator const&) = default;
    recursive_directory_iterator& operator=(recursive_directory_iterator const&) = default;

    recursive_directory_iterator(recursive_directory_iterator&& that) noexcept :
        m_imp(static_cast< boost::intrusive_ptr< detail::recur_dir_itr_imp >&& >(that.m_imp))
    {
    }

    recursive_directory_iterator& operator=(recursive_directory_iterator&& that) noexcept
    {
        m_imp = static_cast< boost::intrusive_ptr< detail::recur_dir_itr_imp >&& >(that.m_imp);
        return *this;
    }

    recursive_directory_iterator& increment(system::error_code& ec) noexcept
    {
        detail::recursive_directory_iterator_increment(*this, &ec);
        return *this;
    }

    int depth() const noexcept
    {
        BOOST_ASSERT_MSG(!is_end(), "depth() on end recursive_directory_iterator");
        return static_cast< int >(m_imp->m_stack.size() - 1u);
    }

    bool recursion_pending() const noexcept
    {
        BOOST_ASSERT_MSG(!is_end(), "recursion_pending() on end recursive_directory_iterator");
        return (m_imp->m_options & directory_options::_detail_no_push) == directory_options::none;
    }

    void pop()
    {
        detail::recursive_directory_iterator_pop(*this, nullptr);
    }

    void pop(system::error_code& ec) noexcept
    {
        detail::recursive_directory_iterator_pop(*this, &ec);
    }

    void disable_recursion_pending(bool value = true) noexcept
    {
        BOOST_ASSERT_MSG(!is_end(), "disable_recursion_pending() on end recursive_directory_iterator");
        if (value)
            m_imp->m_options |= directory_options::_detail_no_push;
        else
            m_imp->m_options &= ~directory_options::_detail_no_push;
    }

    file_status status() const
    {
        BOOST_ASSERT_MSG(!is_end(), "status() on end recursive_directory_iterator");
        return m_imp->m_stack.back()->status();
    }

    file_status symlink_status() const
    {
        BOOST_ASSERT_MSG(!is_end(), "symlink_status() on end recursive_directory_iterator");
        return m_imp->m_stack.back()->symlink_status();
    }

private:
    boost::iterator_facade<
        recursive_directory_iterator,
        directory_entry,
        boost::single_pass_traversal_tag
    >::reference dereference() const
    {
        BOOST_ASSERT_MSG(!is_end(), "dereference of end recursive_directory_iterator");
        return *m_imp->m_stack.back();
    }

    void increment() { detail::recursive_directory_iterator_increment(*this, nullptr); }

    bool equal(recursive_directory_iterator const& rhs) const noexcept
    {
        return m_imp == rhs.m_imp || (is_end() && rhs.is_end());
    }

    bool is_end() const noexcept
    {
        // Note: The check for m_stack.empty() is needed because the iterator can be copied and the copy
        // can be incremented to end while the original iterator still refers to the same recur_dir_itr_imp.
        return !m_imp || m_imp->m_stack.empty();
    }

private:
    // intrusive_ptr provides the shallow-copy semantics required for single pass iterators
    // (i.e. InputIterators). The end iterator is indicated by is_end().
    boost::intrusive_ptr< detail::recur_dir_itr_imp > m_imp;
};

//  enable recursive directory iterator C++11 range-base for statement use  ----------//

// begin() and end() are only used by a range-based for statement in the context of
// auto - thus the top-level const is stripped - so returning const is harmless and
// emphasizes begin() is just a pass through.
inline recursive_directory_iterator const& begin(recursive_directory_iterator const& iter) noexcept
{
    return iter;
}

inline recursive_directory_iterator end(recursive_directory_iterator const&) noexcept
{
    return recursive_directory_iterator();
}

// enable C++14 generic accessors for range const iterators
inline recursive_directory_iterator const& cbegin(recursive_directory_iterator const& iter) noexcept
{
    return iter;
}

inline recursive_directory_iterator cend(recursive_directory_iterator const&) noexcept
{
    return recursive_directory_iterator();
}

//  enable recursive directory iterator BOOST_FOREACH  -------------------------------//

inline recursive_directory_iterator& range_begin(recursive_directory_iterator& iter) noexcept
{
    return iter;
}

inline recursive_directory_iterator range_begin(recursive_directory_iterator const& iter) noexcept
{
    return iter;
}

inline recursive_directory_iterator range_end(recursive_directory_iterator&) noexcept
{
    return recursive_directory_iterator();
}

inline recursive_directory_iterator range_end(recursive_directory_iterator const&) noexcept
{
    return recursive_directory_iterator();
}

} // namespace filesystem

//  namespace boost template specializations
template<>
struct range_mutable_iterator< boost::filesystem::recursive_directory_iterator, void >
{
    typedef boost::filesystem::recursive_directory_iterator type;
};

template<>
struct range_const_iterator< boost::filesystem::recursive_directory_iterator, void >
{
    typedef boost::filesystem::recursive_directory_iterator type;
};

} // namespace boost

#include <boost/filesystem/detail/footer.hpp>

#endif // BOOST_FILESYSTEM_DIRECTORY_HPP
