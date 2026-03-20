/*
 * Copyright (c) 2017-2020 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <limits.h>
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#else
#include "uniwin.h"
#endif
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdexcept>
#include <inttypes.h>

#include "config.h"
#include "rnpcfg.h"
#include "defaults.h"
#include "utils.h"
#include "time-utils.h"
#include <rnp/rnp.h>

// must be placed after include "utils.h"
#ifndef RNP_USE_STD_REGEX
#include <regex.h>
#else
#include <regex>
#endif

typedef enum rnp_cfg_val_type_t {
    RNP_CFG_VAL_NULL = 0,
    RNP_CFG_VAL_INT = 1,
    RNP_CFG_VAL_BOOL = 2,
    RNP_CFG_VAL_STRING = 3,
    RNP_CFG_VAL_LIST = 4
} rnp_cfg_val_type_t;

class rnp_cfg_val {
    rnp_cfg_val_type_t type_;

  public:
    rnp_cfg_val(rnp_cfg_val_type_t t) : type_(t){};
    rnp_cfg_val_type_t
    type() const
    {
        return type_;
    };

    virtual ~rnp_cfg_val(){};
};

class rnp_cfg_int_val : public rnp_cfg_val {
    int val_;

  public:
    rnp_cfg_int_val(int val) : rnp_cfg_val(RNP_CFG_VAL_INT), val_(val){};
    int
    val() const
    {
        return val_;
    };
};

class rnp_cfg_bool_val : public rnp_cfg_val {
    bool val_;

  public:
    rnp_cfg_bool_val(bool val) : rnp_cfg_val(RNP_CFG_VAL_BOOL), val_(val){};
    bool
    val() const
    {
        return val_;
    };
};

class rnp_cfg_str_val : public rnp_cfg_val {
    std::string val_;

  public:
    rnp_cfg_str_val(const std::string &val) : rnp_cfg_val(RNP_CFG_VAL_STRING), val_(val){};
    const std::string &
    val() const
    {
        return val_;
    };
};

class rnp_cfg_list_val : public rnp_cfg_val {
    std::vector<std::string> val_;

  public:
    rnp_cfg_list_val() : rnp_cfg_val(RNP_CFG_VAL_LIST), val_(){};
    std::vector<std::string> &
    val()
    {
        return val_;
    };
    const std::vector<std::string> &
    val() const
    {
        return val_;
    };
};

void
rnp_cfg::load_defaults()
{
    set_bool(CFG_OVERWRITE, false);
    set_str(CFG_OUTFILE, "");
    set_str(CFG_ZALG, DEFAULT_Z_ALG);
    set_int(CFG_ZLEVEL, DEFAULT_Z_LEVEL);
    set_str(CFG_CIPHER, DEFAULT_SYMM_ALG);
    set_int(CFG_NUMTRIES, MAX_PASSWORD_ATTEMPTS);
    set_int(CFG_S2K_MSEC, DEFAULT_S2K_MSEC);
}

void
rnp_cfg::set_str(const std::string &key, const std::string &val)
{
    unset(key);
    vals_[key] = new rnp_cfg_str_val(val);
}

void
rnp_cfg::set_str(const std::string &key, const char *val)
{
    unset(key);
    vals_[key] = new rnp_cfg_str_val(val);
}

void
rnp_cfg::set_int(const std::string &key, int val)
{
    unset(key);
    vals_[key] = new rnp_cfg_int_val(val);
}

void
rnp_cfg::set_bool(const std::string &key, bool val)
{
    unset(key);
    vals_[key] = new rnp_cfg_bool_val(val);
}

void
rnp_cfg::unset(const std::string &key)
{
    if (!vals_.count(key)) {
        return;
    }
    delete vals_[key];
    vals_.erase(key);
}

void
rnp_cfg::add_str(const std::string &key, const std::string &val)
{
    if (!vals_.count(key)) {
        vals_[key] = new rnp_cfg_list_val();
    }
    if (vals_[key]->type() != RNP_CFG_VAL_LIST) {
        RNP_LOG("expected list val for \"%s\"", key.c_str());
        throw std::invalid_argument("type");
    }
    (dynamic_cast<rnp_cfg_list_val &>(*vals_[key])).val().push_back(val);
}

bool
rnp_cfg::has(const std::string &key) const
{
    return vals_.count(key);
}

const std::string &
rnp_cfg::get_str(const std::string &key) const
{
    if (!has(key) || (vals_.at(key)->type() != RNP_CFG_VAL_STRING)) {
        return empty_str_;
    }
    return (dynamic_cast<const rnp_cfg_str_val &>(*vals_.at(key))).val();
}

const char *
rnp_cfg::get_cstr(const std::string &key) const
{
    if (!has(key) || (vals_.at(key)->type() != RNP_CFG_VAL_STRING)) {
        return NULL;
    }
    return (dynamic_cast<const rnp_cfg_str_val &>(*vals_.at(key))).val().c_str();
}

int
rnp_cfg::get_int(const std::string &key, int def) const
{
    if (!has(key)) {
        return def;
    }
    const rnp_cfg_val *val = vals_.at(key);
    switch (val->type()) {
    case RNP_CFG_VAL_INT:
        return (dynamic_cast<const rnp_cfg_int_val &>(*val)).val();
    case RNP_CFG_VAL_BOOL:
        return (dynamic_cast<const rnp_cfg_bool_val &>(*val)).val();
    case RNP_CFG_VAL_STRING:
        return atoi((dynamic_cast<const rnp_cfg_str_val &>(*val)).val().c_str());
    default:
        return def;
    }
}

bool
rnp_cfg::get_bool(const std::string &key) const
{
    if (!has(key)) {
        return false;
    }
    const rnp_cfg_val *val = vals_.at(key);
    switch (val->type()) {
    case RNP_CFG_VAL_INT:
        return (dynamic_cast<const rnp_cfg_int_val &>(*val)).val();
    case RNP_CFG_VAL_BOOL:
        return (dynamic_cast<const rnp_cfg_bool_val &>(*val)).val();
    case RNP_CFG_VAL_STRING: {
        const std::string &str = (dynamic_cast<const rnp_cfg_str_val &>(*val)).val();
        return !strcasecmp(str.c_str(), "true") || (atoi(str.c_str()) > 0);
    }
    default:
        return false;
    }
}

size_t
rnp_cfg::get_count(const std::string &key) const
{
    if (!has(key) || (vals_.at(key)->type() != RNP_CFG_VAL_LIST)) {
        return 0;
    }
    return (dynamic_cast<const rnp_cfg_list_val &>(*vals_.at(key))).val().size();
}

const std::string &
rnp_cfg::get_str(const std::string &key, size_t idx) const
{
    if (get_count(key) <= idx) {
        RNP_LOG("idx is out of bounds for \"%s\"", key.c_str());
        throw std::invalid_argument("idx");
    }
    return (dynamic_cast<const rnp_cfg_list_val &>(*vals_.at(key))).val().at(idx);
}

std::vector<std::string>
rnp_cfg::get_list(const std::string &key) const
{
    if (!has(key)) {
        /* it's okay to return empty list */
        return std::vector<std::string>();
    }
    if (vals_.at(key)->type() != RNP_CFG_VAL_LIST) {
        RNP_LOG("no list at the key \"%s\"", key.c_str());
        throw std::invalid_argument("key");
    }
    return (dynamic_cast<const rnp_cfg_list_val &>(*vals_.at(key))).val();
}

int
rnp_cfg::get_pswdtries() const
{
    auto &numtries = get_str(CFG_NUMTRIES);
    int   num = atoi(numtries.c_str());
    if (numtries.empty() || (num <= 0)) {
        return MAX_PASSWORD_ATTEMPTS;
    } else if (numtries == "unlimited") {
        return INFINITE_ATTEMPTS;
    }
    return num;
}

const std::string
rnp_cfg::get_hashalg() const
{
    auto &hash_alg = get_str(CFG_HASH);
    if (!hash_alg.empty()) {
        return hash_alg;
    }
    return DEFAULT_HASH_ALG;
}

const std::string
rnp_cfg::get_cipher() const
{
    auto &cipher_alg = get_str(CFG_CIPHER);
    if (!cipher_alg.empty()) {
        return cipher_alg;
    }
    return DEFAULT_SYMM_ALG;
}

bool
rnp_cfg::get_expiration(const std::string &key, uint32_t &seconds) const
{
    if (!has(key)) {
        return false;
    }
    const std::string &val = get_str(key);
    uint64_t           delta;
    uint64_t           t;
    if (parse_date(val, t)) {
        uint64_t now = time();
        if (t > now) {
            delta = t - now;
            if (delta > UINT32_MAX) {
                RNP_LOG("Expiration time exceeds 32-bit value");
                return false;
            }
            seconds = delta;
            return true;
        }
        return false;
    }
    const char *reg = "^([0-9]+)([hdwmy]?)$";
#ifndef RNP_USE_STD_REGEX
    static regex_t r;
    static int     compiled;
    regmatch_t     matches[3];

    if (!compiled) {
        compiled = 1;
        if (regcomp(&r, reg, REG_EXTENDED | REG_ICASE)) {
            RNP_LOG("failed to compile regexp");
            return false;
        }
    }
    if (regexec(&r, val.c_str(), ARRAY_SIZE(matches), matches, 0)) {
        return false;
    }
    auto delta_str = &val.c_str()[matches[1].rm_so];
    char mult = val.c_str()[matches[2].rm_so];
#else
    static std::regex re(reg, std::regex_constants::extended | std::regex_constants::icase);
    std::smatch       result;

    if (!std::regex_search(val, result, re)) {
        return false;
    }
    std::string delta_stdstr = result[1].str();
    const char *delta_str = delta_stdstr.c_str();
    char        mult = result[2].str()[0];
#endif
    errno = 0;
    delta = strtoul(delta_str, NULL, 10);
    if (errno || delta > UINT_MAX) {
        RNP_LOG("Invalid expiration '%s'.", delta_str);
        return false;
    }
    switch (std::tolower(mult)) {
    case 'h':
        delta *= 60 * 60;
        break;
    case 'd':
        delta *= 60 * 60 * 24;
        break;
    case 'w':
        delta *= 60 * 60 * 24 * 7;
        break;
    case 'm':
        delta *= 60 * 60 * 24 * 31;
        break;
    case 'y':
        delta *= 60 * 60 * 24 * 365;
        break;
    }
    if (delta > UINT32_MAX) {
        RNP_LOG("Expiration value exceed 32 bit.");
        return false;
    }
    seconds = delta;
    return true;
}

bool
rnp_cfg::extract_timestamp(const std::string &st, uint64_t &t) const
{
    if (st.empty()) {
        return false;
    }
    if (parse_date(st, t)) {
        return true;
    }
    /* Check if string is UNIX timestamp */
    for (auto c : st) {
        if (!isdigit(c)) {
            return false;
        }
    }
    t = std::stoll(st);
    return true;
}

uint64_t
rnp_cfg::get_sig_creation() const
{
    uint64_t t = 0;
    if (extract_timestamp(get_str(CFG_CREATION), t)) {
        return t;
    }
    return time();
}

uint64_t
rnp_cfg::time() const
{
    uint64_t t = 0;
    if (extract_timestamp(get_str(CFG_CURTIME), t)) {
        return t;
    }
    return ::time(NULL);
}

void
rnp_cfg::copy(const rnp_cfg &src)
{
    for (const auto &it : src.vals_) {
        if (has(it.first)) {
            unset(it.first);
        }
        rnp_cfg_val *val = NULL;
        switch (it.second->type()) {
        case RNP_CFG_VAL_INT:
            val = new rnp_cfg_int_val(dynamic_cast<const rnp_cfg_int_val &>(*it.second));
            break;
        case RNP_CFG_VAL_BOOL:
            val = new rnp_cfg_bool_val(dynamic_cast<const rnp_cfg_bool_val &>(*it.second));
            break;
        case RNP_CFG_VAL_STRING:
            val = new rnp_cfg_str_val(dynamic_cast<const rnp_cfg_str_val &>(*it.second));
            break;
        case RNP_CFG_VAL_LIST:
            val = new rnp_cfg_list_val(dynamic_cast<const rnp_cfg_list_val &>(*it.second));
            break;
        default:
            continue;
        }
        vals_[it.first] = val;
    }
}

void
rnp_cfg::clear()
{
    for (const auto &it : vals_) {
        delete it.second;
    }
    vals_.clear();
}

rnp_cfg::~rnp_cfg()
{
    clear();
}

/**
 * @brief Get number of days in month.
 *
 * @param year number of year, i.e. 2021
 * @param month number of month, 1..12
 * @return number of days (28..31) or 0 if month is wrong.
 */
static int
days_in_month(int year, int month)
{
    switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
        return 31;
    case 4:
    case 6:
    case 9:
    case 11:
        return 30;
    case 2: {
        bool leap_year = !(year % 400) || (!(year % 4) && (year % 100));
        return leap_year ? 29 : 28;
    }
    default:
        return 0;
    }
}

bool
rnp_cfg::parse_date(const std::string &s, uint64_t &t) const
{
    /* fill time zone information */
    struct tm tm;
    rnp_localtime(::time(NULL), tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    const char *reg = "^([0-9]{4})[-/\\.]([0-9]{2})[-/\\.]([0-9]{2})$";
    int         year = 0, mon = 0, mday = 0;
#ifndef RNP_USE_STD_REGEX
    static regex_t r;
    static int     compiled;

    if (!compiled) {
        compiled = 1;
        if (regcomp(&r, reg, REG_EXTENDED)) {
            /* LCOV_EXCL_START */
            RNP_LOG("failed to compile regexp");
            return false;
            /* LCOV_EXCL_END */
        }
    }
    regmatch_t matches[4];
    if (regexec(&r, s.c_str(), ARRAY_SIZE(matches), matches, 0)) {
        return false;
    }
    year = strtol(&s[matches[1].rm_so], NULL, 10);
    mon = strtol(&s[matches[2].rm_so], NULL, 10);
    mday = strtol(&s[matches[3].rm_so], NULL, 10);
#else
    try {
        static std::regex re(reg, std::regex_constants::extended);
        std::smatch       result;

        if (!std::regex_search(s, result, re)) {
            return false;
        }
        year = std::stoi(result[1].str());
        mon = std::stoi(result[2].str());
        mday = std::stoi(result[3].str());
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("got regex exception: %s", e.what());
        return false;
        /* LCOV_EXCL_END */
    }
#endif
    if (year < 1970 || mon < 1 || mon > 12 || !mday || (mday > days_in_month(year, mon))) {
        RNP_LOG("invalid date: %s.", s.c_str());
        return false;
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = mday;
    /* line below is required to correctly handle DST changes */
    tm.tm_isdst = -1;

    struct tm check_tm = tm;
    time_t    built_time = rnp_mktime(&tm);
    time_t    check_time = mktime(&check_tm);
    if (built_time != check_time) {
        /* If date is beyond of yk2038 and we have 32-bit signed time_t, we need to reduce
         * timestamp */
        RNP_LOG("Warning: date %s is beyond of 32-bit time_t, so timestamp was reduced to "
                "maximum supported value.",
                s.c_str());
    }
    t = built_time;
    return true;
}
