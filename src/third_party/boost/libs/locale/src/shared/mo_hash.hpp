//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <cstdint>

namespace boost { namespace locale { namespace gnu_gettext {

    struct pj_winberger_hash {
        typedef uint32_t state_type;

        static constexpr state_type initial_state = 0;

        static state_type update_state(state_type value, char c)
        {
            value = (value << 4) + static_cast<unsigned char>(c);
            uint32_t high = (value & 0xF0000000U);
            if(high != 0)
                value = (value ^ (high >> 24)) ^ high;
            return value;
        }
        static state_type update_state(state_type value, const char* ptr)
        {
            while(*ptr)
                value = update_state(value, *ptr++);
            return value;
        }
        static state_type update_state(state_type value, const char* begin, const char* end)
        {
            while(begin != end)
                value = update_state(value, *begin++);
            return value;
        }
    };

    inline pj_winberger_hash::state_type pj_winberger_hash_function(const char* ptr)
    {
        pj_winberger_hash::state_type state = pj_winberger_hash::initial_state;
        state = pj_winberger_hash::update_state(state, ptr);
        return state;
    }

    inline pj_winberger_hash::state_type pj_winberger_hash_function(const char* begin, const char* end)
    {
        pj_winberger_hash::state_type state = pj_winberger_hash::initial_state;
        state = pj_winberger_hash::update_state(state, begin, end);
        return state;
    }
}}} // namespace boost::locale::gnu_gettext
