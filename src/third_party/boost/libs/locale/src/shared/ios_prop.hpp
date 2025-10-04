//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_SRC_LOCALE_IOS_PROP_HPP
#define BOOST_SRC_LOCALE_IOS_PROP_HPP

#include <boost/locale/config.hpp>
#include <boost/assert.hpp>
#include <ios>

namespace boost { namespace locale { namespace impl {

    template<typename Property>
    class BOOST_SYMBOL_VISIBLE ios_prop {
    public:
        static Property& get(std::ios_base& ios)
        {
            Property* result = get_impl(ios);
            return result ? *result : create(ios);
        }

        static void global_init() { get_id(); }

    private:
        static Property* get_impl(std::ios_base& ios) { return static_cast<Property*>(ios.pword(get_id())); }

        static Property& create(std::ios_base& ios)
        {
            BOOST_ASSERT_MSG(!get_impl(ios), "Called create while the property already exists");
            const int id = get_id();
            ios.register_callback(callback, id);
            Property* value = new Property();
            ios.pword(id) = value;
            return *value;
        }

        static void callback(std::ios_base::event ev, std::ios_base& ios, int id)
        {
            Property* prop = get_impl(ios);
            if(!prop)
                return;
            switch(ev) {
                case std::ios_base::erase_event:
                    delete prop;
                    ios.pword(id) = nullptr;
                    break;
                case std::ios_base::copyfmt_event: ios.pword(id) = new Property(*prop); break;
                case std::ios_base::imbue_event: prop->on_imbue(); break;
                default: break;
            }
        }
        static int get_id()
        {
            static int id = std::ios_base::xalloc();
            return id;
        }
    };

}}} // namespace boost::locale::impl

#endif
