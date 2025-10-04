//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_LOCALIZATION_BACKEND_HPP
#define BOOST_LOCALE_LOCALIZATION_BACKEND_HPP

#include <boost/locale/generator.hpp>
#include <boost/locale/hold_ptr.hpp>
#include <locale>
#include <memory>
#include <string>
#include <vector>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale {

    /// \brief this class represents a localization backend that can be used for localizing your application.
    ///
    /// Backends are usually registered inside the localization backends manager and allow transparent support
    /// of different backends, so a user can switch the backend by simply linking the application to the correct one.
    ///
    /// Backends may support different tuning options, but these are the default options available to the user
    /// for all of them
    ///
    /// -# \c locale - the name of the locale in POSIX format like en_US.UTF-8
    /// -# \c use_ansi_encoding - select system locale using ANSI codepages rather then UTF-8 under Windows
    ///     by default
    /// -# \c message_path - path to the location of message catalogs (vector of strings)
    /// -# \c message_application - the name of applications that use message catalogs (vector of strings)
    ///
    /// Each backend can be installed with a different default priority so when you work with two different backends,
    /// you can specify priority so this backend will be chosen according to their priority.
    class BOOST_LOCALE_DECL localization_backend {
    protected:
        localization_backend(const localization_backend&) = default;
        localization_backend& operator=(const localization_backend&) = default;

    public:
        localization_backend() = default;
        virtual ~localization_backend();

        /// Make a polymorphic copy of the backend
        virtual localization_backend* clone() const = 0;

        /// Set option for backend, for example "locale" or "encoding"
        virtual void set_option(const std::string& name, const std::string& value) = 0;

        /// Clear all options
        virtual void clear_options() = 0;

        /// Create a facet for category \a category and character type \a type
        virtual std::locale install(const std::locale& base, category_t category, char_facet_t type) = 0;

    }; // localization_backend

    /// \brief Localization backend manager is a class that holds various backend and allows creation
    /// of their combination or selection
    class BOOST_LOCALE_DECL localization_backend_manager {
    public:
        /// New empty localization_backend_manager
        localization_backend_manager();
        /// Copy localization_backend_manager
        localization_backend_manager(const localization_backend_manager&);
        /// Assign localization_backend_manager
        localization_backend_manager& operator=(const localization_backend_manager&);
        /// Move construct localization_backend_manager
        localization_backend_manager(localization_backend_manager&&) noexcept;
        /// Move assign localization_backend_manager
        localization_backend_manager& operator=(localization_backend_manager&&) noexcept;

        /// Destructor
        ~localization_backend_manager();

        /// Create new localization backend according to current settings. Ownership is passed to caller
        std::unique_ptr<localization_backend> create() const;

        BOOST_DEPRECATED("This function is deprecated, use 'create()' instead")
        std::unique_ptr<localization_backend> get() const { return create(); } // LCOV_EXCL_LINE
        BOOST_DEPRECATED("This function is deprecated, use 'create()' instead")
        std::unique_ptr<localization_backend> get_unique_ptr() const { return create(); } // LCOV_EXCL_LINE

        /// Add new backend to the manager, each backend should be uniquely defined by its name.
        ///
        /// This library provides: "icu", "posix", "winapi" and "std" backends.
        void add_backend(const std::string& name, std::unique_ptr<localization_backend> backend);

        // clang-format off
        BOOST_DEPRECATED("This function is deprecated, use 'add_backend' instead")
        void adopt_backend(const std::string& name, localization_backend* backend) { add_backend(name, std::unique_ptr<localization_backend>(backend)); } // LCOV_EXCL_LINE
        // clang-format on

        /// Clear backend
        void remove_all_backends();

        /// Get list of all available backends
        std::vector<std::string> get_all_backends() const;

        /// Select specific backend by name for a category \a category. It allows combining different
        /// backends for user preferences.
        void select(const std::string& backend_name, category_t category = all_categories);

        /// Set new global backend manager, the old one is returned.
        ///
        /// This function is thread safe
        static localization_backend_manager global(const localization_backend_manager&);
        /// Get global backend manager
        ///
        /// This function is thread safe
        static localization_backend_manager global();

    private:
        class impl;
        hold_ptr<impl> pimpl_;
    };

}} // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
