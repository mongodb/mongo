//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_GNU_GETTEXT_HPP
#define BOOST_LOCALE_GNU_GETTEXT_HPP

#include <boost/locale/detail/is_supported_char.hpp>
#include <boost/locale/message.hpp>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <vector>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4251) // "identifier" : class "type" needs to have dll-interface...
#endif

namespace boost { namespace locale {
    /// \addtogroup message
    /// @{

    /// \brief This namespace holds classes that provide GNU Gettext message catalogs support.
    namespace gnu_gettext {

        /// \brief This structure holds all information required for creating gnu-gettext message catalogs,
        ///
        /// The user is expected to set its parameters to load these catalogs correctly. This structure
        /// also allows providing functions for charset conversion. Note, you need to provide them,
        /// so this structure is not useful for wide characters without subclassing and it will also
        /// ignore gettext catalogs that use a charset different from \a encoding.
        struct BOOST_LOCALE_DECL messages_info {
            messages_info() : language("C"), locale_category("LC_MESSAGES") {}

            std::string language; ///< The language we load the catalog for, like "ru", "en", "de"
            std::string country;  ///< The country we load the catalog for, like "US", "IL"
            std::string variant;  ///< Language variant, like "euro" so it would look for catalog like de_DE\@euro
            std::string encoding; ///< Required target charset encoding. Ignored for wide characters.
                                  ///< For narrow, should specify the correct encoding required for this catalog
            std::string locale_category; ///< Locale category, is set by default to LC_MESSAGES, but may be changed
            ///
            /// \brief This type represents GNU Gettext domain name for the messages.
            ///
            /// It consists of two parameters:
            ///
            /// - name - the name of the domain - used for opening the file name
            /// - encoding - the encoding of the keys in the sources, default - UTF-8
            ///
            struct domain {
                std::string name;     ///< The name of the domain
                std::string encoding; ///< The character encoding for the domain
                domain() = default;

                /// Create a domain object from the name that can hold an encoding after symbol "/"
                /// such that if n is "hello/cp1255" then the name would be "hello" and "encoding" would
                /// be "cp1255" and if n is "hello" then the name would be the same but encoding would be
                /// "UTF-8"
                domain(const std::string& n)
                {
                    const size_t pos = n.find('/');
                    if(pos == std::string::npos) {
                        name = n;
                        encoding = "UTF-8";
                    } else {
                        name = n.substr(0, pos);
                        encoding = n.substr(pos + 1);
                    }
                }

                /// Check whether two objects are equivalent, only names are compared, encoding is ignored
                bool operator==(const domain& other) const { return name == other.name; }
                /// Check whether two objects are distinct, only names are compared, encoding is ignored
                bool operator!=(const domain& other) const { return !(*this == other); }
            };

            typedef std::vector<domain> domains_type; ///< Type that defines a list of domains that are loaded
                                                      ///< The first one is the default one
            domains_type domains; ///< Message domains - application name, like my_app. So files named my_app.mo
                                  ///< would be loaded
            std::vector<std::string> paths; ///< Paths to search files in. Under MS Windows it uses encoding
                                            ///< parameter to convert them to wide OS specific paths.

            /// The callback for custom file system support. This callback should read the file named \a file_name
            /// encoded in \a encoding character set into std::vector<char> and return it.
            ///
            /// - If the file does not exist, it should return an empty vector.
            /// - If an error occurs during file read it should throw an exception.
            ///
            /// \note The user should support only the encodings the locales are created for. So if the user
            /// uses only one encoding or the file system is encoding agnostic, he may ignore the \a encoding parameter.
            typedef std::function<std::vector<char>(const std::string& file_name, const std::string& encoding)>
              callback_type;

            /// The callback for handling custom file systems, if it is empty, the real OS file-system
            /// is being used.
            callback_type callback;

            /// Get paths to folders which may contain catalog files
            std::vector<std::string> get_catalog_paths() const;

        private:
            /// Get a list of folder names for the language, country and variant
            std::vector<std::string> get_lang_folders() const;
        };

        /// Create a message_format facet using GNU Gettext catalogs. It uses \a info structure to get
        /// information about where to read them from and uses it for character set conversion (if needed)
        template<typename CharType, class = boost::locale::detail::enable_if_is_supported_char<CharType>>
        BOOST_LOCALE_DECL message_format<CharType>* create_messages_facet(const messages_info& info);

    } // namespace gnu_gettext

    /// @}

}} // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
