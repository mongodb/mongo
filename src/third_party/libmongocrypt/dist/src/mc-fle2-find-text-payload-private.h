/*
 * Copyright 2025-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MC_FLE2_FIND_TEXT_PAYLOAD_PRIVATE_H
#define MC_FLE2_FIND_TEXT_PAYLOAD_PRIVATE_H

#include "mc-fle2-encryption-placeholder-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt.h"

#define DEF_TEXT_SEARCH_FIND_TOKEN_SET(Type)                                                                           \
    typedef struct {                                                                                                   \
        _mongocrypt_buffer_t edcDerivedToken;                                                                          \
        _mongocrypt_buffer_t escDerivedToken;                                                                          \
        _mongocrypt_buffer_t serverDerivedFromDataToken;                                                               \
    } mc_Text##Type##FindTokenSet_t;

DEF_TEXT_SEARCH_FIND_TOKEN_SET(Exact);
DEF_TEXT_SEARCH_FIND_TOKEN_SET(Substring);
DEF_TEXT_SEARCH_FIND_TOKEN_SET(Suffix);
DEF_TEXT_SEARCH_FIND_TOKEN_SET(Prefix);

typedef struct {
    struct {
        mc_TextExactFindTokenSet_t value;
        bool set;
    } exact; // e

    struct {
        mc_TextSubstringFindTokenSet_t value;
        bool set;
    } substring; // s

    struct {
        mc_TextSuffixFindTokenSet_t value;
        bool set;
    } suffix; // u

    struct {
        mc_TextPrefixFindTokenSet_t value;
        bool set;
    } prefix; // p
} mc_TextSearchFindTokenSets_t;

/**
 * FLE2FindTextPayload represents an FLE2 payload of a substring/suffix/prefix indexed field to
 * query. It is created client side.
 *
 * FLE2FindTextPayload has the following data layout:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 18;
 *   uint8_t bson[];
 * } FLE2FindTextPayload;
 *
 * bson is a BSON document of this form:
 * {
 *   ts: {
 *      e: optional<TextExactFindTokenSet>      // required tokens if doing full string match
 *      s: optional<TextSubstringFindTokenSet>  // required tokens if doing substring match
 *      u: optional<TextSuffixFindTokenSet>     // required tokens if doing suffix match
 *      p: optional<TextPrefixFindTokenSet>     // required tokens if doing prefix match
 *   }
 *   cm: <int64>                                // Queryable Encryption max contentionFactor
 *   cf: <bool>                                 // case folding parameter
 *   df: <bool>                                 // diacritic folding parameter
 *   ss: optional<FLE2SubstringInsertSpec>      // substring-indexing parameters (if applicable)
 *   fs: optional<FLE2SuffixInsertSpec>         // suffix-indexing parameters (if applicable)
 *   ps: optional<FLE2PrefixInsertSpec>         // prefix-indexing parameters (if applicable)
 * }
 * where for each of T in [Exact, Substring, Suffix, Prefix], Text<T>FindTokenSet is a document of form:
 * {
 *    d: <binary>                               // EDCText<T>DerivedFromDataToken
 *    s: <binary>                               // ESCText<T>DerivedFromDataToken
 *    l: <binary>                               // ServerText<T>DerivedFromDataToken
 * }
 */
typedef struct {
    mc_TextSearchFindTokenSets_t tokenSets; // ts
    int64_t maxContentionFactor;            // cm
    bool caseFold;                          // cf
    bool diacriticFold;                     // df

    struct {
        mc_FLE2SubstringInsertSpec_t value;
        bool set;
    } substringSpec; // ss

    struct {
        mc_FLE2SuffixInsertSpec_t value;
        bool set;
    } suffixSpec; // fs

    struct {
        mc_FLE2PrefixInsertSpec_t value;
        bool set;
    } prefixSpec; // ps
} mc_FLE2FindTextPayload_t;

void mc_FLE2FindTextPayload_init(mc_FLE2FindTextPayload_t *payload);

bool mc_FLE2FindTextPayload_parse(mc_FLE2FindTextPayload_t *out, const bson_t *in, mongocrypt_status_t *status);

bool mc_FLE2FindTextPayload_serialize(const mc_FLE2FindTextPayload_t *payload, bson_t *out);

void mc_FLE2FindTextPayload_cleanup(mc_FLE2FindTextPayload_t *payload);

#endif /* MC_FLE2_FIND_TEXT_PAYLOAD_PRIVATE_H */
