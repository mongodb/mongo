//
// Copyright (c) 2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_STD_COLLATE_ADAPTER_HPP
#define BOOST_LOCALE_STD_COLLATE_ADAPTER_HPP

#include <boost/locale/collator.hpp>
#include <locale>
#include <utility>

namespace boost { namespace locale { namespace impl {

    template<typename CharT, class Base>
    class BOOST_SYMBOL_VISIBLE std_collate_adapter : public std::collate<CharT> {
    public:
        using typename std::collate<CharT>::string_type;

        template<typename... TArgs>
        explicit std_collate_adapter(TArgs&&... args) : base_(std::forward<TArgs>(args)...)
        {}

    protected:
        int do_compare(const CharT* beg1, const CharT* end1, const CharT* beg2, const CharT* end2) const override
        {
            return base_.compare(collate_level::identical, beg1, end1, beg2, end2);
        }

        string_type do_transform(const CharT* beg, const CharT* end) const override
        {
            return base_.transform(collate_level::identical, beg, end);
        }
        long do_hash(const CharT* beg, const CharT* end) const override
        {
            return base_.hash(collate_level::identical, beg, end);
        }
        Base base_;
    };

    template<typename CharType, class CollatorImpl, typename... TArgs>
    static std::locale create_collators(const std::locale& in, TArgs&&... args)
    {
        static_assert(std::is_base_of<collator<CharType>, CollatorImpl>::value, "Must be a collator implementation");
        std::locale res(in, new CollatorImpl(args...));
        return std::locale(res, new std_collate_adapter<CharType, CollatorImpl>(args...));
    }

    template<typename CharType, template<typename> class CollatorImpl, typename... TArgs>
    static std::locale create_collators(const std::locale& in, TArgs&&... args)
    {
        return create_collators<CharType, CollatorImpl<CharType>>(in, args...);
    }

}}} // namespace boost::locale::impl

#endif
