//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_COLLATOR_HPP_INCLUDED
#define BOOST_LOCALE_COLLATOR_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <boost/locale/detail/facet_id.hpp>
#include <locale>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale {

    /// \defgroup collation Collation
    ///
    /// This module introduces collation related classes
    /// @{

    /// Unicode collation level types
    enum class collate_level {
        primary = 0,    ///< 1st collation level: base letters
        secondary = 1,  ///< 2nd collation level: letters and accents
        tertiary = 2,   ///< 3rd collation level: letters, accents and case
        quaternary = 3, ///< 4th collation level: letters, accents, case and punctuation
        identical = 4   ///< identical collation level: include code-point comparison
    };

    class BOOST_DEPRECATED("Use collate_level") collator_base {
    public:
        using level_type = collate_level;
        static constexpr auto primary = collate_level::primary;
        static constexpr auto secondary = collate_level::secondary;
        static constexpr auto tertiary = collate_level::tertiary;
        static constexpr auto quaternary = collate_level::quaternary;
        static constexpr auto identical = collate_level::identical;
    };

    /// \brief Collation facet.
    ///
    /// It reimplements standard C++ std::collate with support for collation levels
    template<typename CharType>
    class BOOST_SYMBOL_VISIBLE collator : public std::locale::facet, public detail::facet_id<collator<CharType>> {
    public:
        /// Type of the underlying character
        typedef CharType char_type;
        /// Type of string used with this facet
        typedef std::basic_string<CharType> string_type;

        /// Compare two strings in range [b1,e1),  [b2,e2) according to collation level \a level. Calls do_compare
        ///
        /// Returns -1 if the first of the two strings sorts before the seconds, returns 1 if sorts after and 0 if
        /// they considered equal.
        int compare(collate_level level,
                    const char_type* b1,
                    const char_type* e1,
                    const char_type* b2,
                    const char_type* e2) const
        {
            return do_compare(level, b1, e1, b2, e2);
        }

        /// Default compare function as-in std::collate that does not take collation level into account.
        /// Uses identical level
        int compare(const char_type* b1, const char_type* e1, const char_type* b2, const char_type* e2) const
        {
            return compare(collate_level::identical, b1, e1, b2, e2);
        }

        /// Create a binary string that can be compared to other in order to get collation order. The string is created
        /// for text in range [b,e). It is useful for collation of multiple strings for text.
        ///
        /// The transformation follows these rules:
        /// \code
        ///   compare(level,b1,e1,b2,e2) == sign( transform(level,b1,e1).compare(transform(level,b2,e2)) );
        /// \endcode
        ///
        /// Calls do_transform
        string_type transform(collate_level level, const char_type* b, const char_type* e) const
        {
            return do_transform(level, b, e);
        }

        /// Default transform function as-in std::collate that does not take collation level into account.
        /// Uses identical level
        string_type transform(const char_type* b, const char_type* e) const
        {
            return transform(collate_level::identical, b, e);
        }

        /// Calculate a hash of a text in range [b,e). The value can be used for collation sensitive string comparison.
        ///
        /// If compare(level,b1,e1,b2,e2) == 0 then hash(level,b1,e1) == hash(level,b2,e2)
        ///
        /// Calls do_hash
        long hash(collate_level level, const char_type* b, const char_type* e) const { return do_hash(level, b, e); }

        /// Default hash function as-in std::collate that does not take collation level into account.
        /// Uses identical level
        long hash(const char_type* b, const char_type* e) const { return hash(collate_level::identical, b, e); }

        /// Compare two strings \a l and \a r using collation level \a level
        ///
        /// Returns -1 if the first of the two strings sorts before the seconds, returns 1 if sorts after and 0 if
        /// they considered equal.
        int compare(collate_level level, const string_type& l, const string_type& r) const
        {
            return do_compare(level, l.data(), l.data() + l.size(), r.data(), r.data() + r.size());
        }

        /// Calculate a hash that can be used for collation sensitive string comparison of a string \a s
        ///
        /// If compare(level,s1,s2) == 0 then hash(level,s1) == hash(level,s2)
        long hash(collate_level level, const string_type& s) const
        {
            return do_hash(level, s.data(), s.data() + s.size());
        }

        /// Create a binary string from string \a s, that can be compared to other, useful for collation of multiple
        /// strings.
        ///
        /// The transformation follows this rule:
        /// \code
        ///   compare(level,s1,s2) == sign( transform(level,s1).compare(transform(level,s2)) );
        /// \endcode
        string_type transform(collate_level level, const string_type& s) const
        {
            return do_transform(level, s.data(), s.data() + s.size());
        }

    protected:
        /// constructor of the collator object
        collator(size_t refs = 0) : std::locale::facet(refs) {}

        /// Actual function that performs comparison between the strings. For details see compare member function. Can
        /// be overridden.
        virtual int do_compare(collate_level level,
                               const char_type* b1,
                               const char_type* e1,
                               const char_type* b2,
                               const char_type* e2) const = 0;

        /// Actual function that performs transformation. For details see transform member function. Can be overridden.
        virtual string_type do_transform(collate_level level, const char_type* b, const char_type* e) const = 0;
        /// Actual function that calculates hash. For details see hash member function. Can be overridden.
        virtual long do_hash(collate_level level, const char_type* b, const char_type* e) const = 0;
    };

    /// \brief This class can be used in STL algorithms and containers for comparison of strings
    /// with a level other than identical
    ///
    /// For example:
    ///
    /// \code
    ///  std::map<std::string,std::string,comparator<char,collate_level::secondary> > data;
    /// \endcode
    ///
    /// Would create a map the keys of which are sorted using secondary collation level
    template<typename CharType, collate_level default_level = collate_level::identical>
    struct comparator {
    public:
        /// Create a comparator class for locale \a l and with collation level \a level
        ///
        /// \throws std::bad_cast: \a l does not have \ref collator facet installed
        comparator(const std::locale& l = std::locale(), collate_level level = default_level) :
            locale_(l), collator_(std::use_facet<collator<CharType>>(locale_)), level_(level)
        {}

        /// Compare two strings -- equivalent to return left < right according to collation rules
        bool operator()(const std::basic_string<CharType>& left, const std::basic_string<CharType>& right) const
        {
            return collator_.compare(level_, left, right) < 0;
        }

    private:
        std::locale locale_;
        const collator<CharType>& collator_;
        collate_level level_;
    };

    ///@}
}} // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

///
/// \example collate.cpp
/// Example of using collation functions
///

#endif
