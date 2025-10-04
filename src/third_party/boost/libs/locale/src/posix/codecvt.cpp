//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/encoding_errors.hpp>
#include "../shared/iconv_codecvt.hpp"
#include "../util/encoding.hpp"
#include "all_generator.hpp"
#include <stdexcept>

namespace boost { namespace locale { namespace impl_posix {

    std::locale create_codecvt(const std::locale& in, const std::string& encoding, char_facet_t type)
    {
        if(util::normalize_encoding(encoding) == "utf8")
            return util::create_utf8_codecvt(in, type);

        try {
            return util::create_simple_codecvt(in, encoding, type);
        } catch(const conv::invalid_charset_error&) {
            return util::create_codecvt(in, create_iconv_converter(encoding), type);
        }
    }

}}} // namespace boost::locale::impl_posix
