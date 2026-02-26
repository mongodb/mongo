/*
 * Copyright (c) 2021 [Ribose Inc](https://www.ribose.com).
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

#include "sec_profile.hpp"
#include "types.h"
#include "defaults.h"
#include <ctime>
#include <algorithm>

namespace rnp {
bool
SecurityRule::operator==(const SecurityRule &src) const
{
    return (type == src.type) && (feature == src.feature) && (from == src.from) &&
           (level == src.level) && (override == src.override) && (action == src.action);
}

bool
SecurityRule::operator!=(const SecurityRule &src) const
{
    return !(*this == src);
}

bool
SecurityRule::matches(FeatureType    ftype,
                      int            fval,
                      uint64_t       ftime,
                      SecurityAction faction) const noexcept
{
    if ((type != ftype) || (feature != fval) || (from > ftime)) {
        return false;
    }
    return (action == SecurityAction::Any) || (faction == SecurityAction::Any) ||
           (action == faction);
}

size_t
SecurityProfile::size() const noexcept
{
    return rules_.size();
}

SecurityRule &
SecurityProfile::add_rule(const SecurityRule &rule)
{
    rules_.push_back(rule);
    return rules_.back();
}

SecurityRule &
SecurityProfile::add_rule(SecurityRule &&rule)
{
    rules_.emplace_back(rule);
    return rules_.back();
}

bool
SecurityProfile::del_rule(const SecurityRule &rule)
{
    size_t old_size = rules_.size();
    rules_.erase(std::remove_if(rules_.begin(),
                                rules_.end(),
                                [rule](const SecurityRule &item) { return item == rule; }),
                 rules_.end());
    return old_size != rules_.size();
}

void
SecurityProfile::clear_rules(FeatureType type, int feature)
{
    rules_.erase(std::remove_if(rules_.begin(),
                                rules_.end(),
                                [type, feature](const SecurityRule &item) {
                                    return (item.type == type) && (item.feature == feature);
                                }),
                 rules_.end());
}

void
SecurityProfile::clear_rules(FeatureType type)
{
    rules_.erase(
      std::remove_if(rules_.begin(),
                     rules_.end(),
                     [type](const SecurityRule &item) { return item.type == type; }),
      rules_.end());
}

void
SecurityProfile::clear_rules()
{
    rules_.clear();
}

bool
SecurityProfile::has_rule(FeatureType    type,
                          int            value,
                          uint64_t       time,
                          SecurityAction action) const noexcept
{
    for (auto &rule : rules_) {
        if (rule.matches(type, value, time, action)) {
            return true;
        }
    }
    return false;
}

const SecurityRule &
SecurityProfile::get_rule(FeatureType    type,
                          int            value,
                          uint64_t       time,
                          SecurityAction action) const
{
    const SecurityRule *res = nullptr;
    for (auto &rule : rules_) {
        if (!rule.matches(type, value, time, action)) {
            continue;
        }
        if (rule.override) {
            return rule;
        }
        if (!res || (res->from < rule.from)) {
            res = &rule;
        }
    }
    if (!res) {
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    return *res;
}

SecurityLevel
SecurityProfile::hash_level(pgp_hash_alg_t hash,
                            uint64_t       time,
                            SecurityAction action) const noexcept
{
    if (!has_rule(FeatureType::Hash, hash, time, action)) {
        return def_level();
    }

    try {
        return get_rule(FeatureType::Hash, hash, time, action).level;
    } catch (const std::exception &e) {
        /* this should never happen however we need to satisfy noexcept specifier */
        return def_level();
    }
}

SecurityLevel
SecurityProfile::def_level() const
{
    return SecurityLevel::Default;
};

SecurityContext::SecurityContext() : time_(0), prov_state_(NULL), rng(RNG::Type::DRBG)
{
    /* Initialize crypto provider if needed (currently only for OpenSSL 3.0) */
    if (!rnp::backend_init(&prov_state_)) {
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }
    /* Mark SHA-1 data signature insecure since 2019-01-19, as GnuPG does */
    profile.add_rule({FeatureType::Hash,
                      PGP_HASH_SHA1,
                      SecurityLevel::Insecure,
                      1547856000,
                      SecurityAction::VerifyData});
    /* Mark SHA-1 key signature insecure since 2024-01-19 by default */
    profile.add_rule({FeatureType::Hash,
                      PGP_HASH_SHA1,
                      SecurityLevel::Insecure,
                      1705629600,
                      SecurityAction::VerifyKey});
    /* Mark MD5 insecure since 2012-01-01 */
    profile.add_rule({FeatureType::Hash, PGP_HASH_MD5, SecurityLevel::Insecure, 1325376000});
    /* Mark CAST5, 3DES, IDEA, BLOWFISH insecure since 2024-10-01*/
    profile.add_rule({FeatureType::Cipher, PGP_SA_CAST5, SecurityLevel::Insecure, 1727730000});
    profile.add_rule(
      {FeatureType::Cipher, PGP_SA_TRIPLEDES, SecurityLevel::Insecure, 1727730000});
    profile.add_rule({FeatureType::Cipher, PGP_SA_IDEA, SecurityLevel::Insecure, 1727730000});
    profile.add_rule(
      {FeatureType::Cipher, PGP_SA_BLOWFISH, SecurityLevel::Insecure, 1727730000});
}

SecurityContext::~SecurityContext()
{
    rnp::backend_finish(prov_state_);
}

size_t
SecurityContext::s2k_iterations(pgp_hash_alg_t halg)
{
    if (!s2k_iterations_.count(halg)) {
        s2k_iterations_[halg] =
          pgp_s2k_compute_iters(halg, DEFAULT_S2K_MSEC, DEFAULT_S2K_TUNE_MSEC);
    }
    return s2k_iterations_[halg];
}

void
SecurityContext::set_time(uint64_t time) noexcept
{
    time_ = time;
}

uint64_t
SecurityContext::time() const noexcept
{
    return time_ ? time_ : ::time(NULL);
}

} // namespace rnp
