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

#ifndef MC_TEXTOPTS_PRIVATE_H
#define MC_TEXTOPTS_PRIVATE_H

#include <bson/bson.h>

#include "mc-optional-private.h"
#include "mongocrypt-private.h"

typedef struct {
    bool set;
    mc_optional_int32_t strMaxLength;
    int32_t strMinQueryLength;
    int32_t strMaxQueryLength;
} mc_TextOptsPerIndex_t;

typedef struct {
    mc_TextOptsPerIndex_t substring;
    mc_TextOptsPerIndex_t prefix;
    mc_TextOptsPerIndex_t suffix;

    bool caseSensitive;
    bool diacriticSensitive;
} mc_TextOpts_t;

/* mc_TextOpts_parse parses a BSON document into mc_TextOpts_t.
 * The document is expected to have the form:
 * {
 *   "caseSensitive": bool,
 * . "diacriticSensitive": bool,
 * . "prefix": {
 * .   "strMaxQueryLength": Int32,
 * .   "strMinQueryLength": Int32,
 * . },
 * . "suffix": {
 * .   "strMaxQueryLength": Int32,
 * .   "strMinQueryLength": Int32,
 * . },
 * . "substring": {
 * .   "strMaxLength": Int32,
 * .   "strMaxQueryLength": Int32,
 * .   "strMinQueryLength": Int32,
 * . },
 * }
 */
bool mc_TextOpts_parse(mc_TextOpts_t *txo, const bson_t *in, mongocrypt_status_t *status);

/*
 * mc_TextOpts_to_FLE2TextSearchInsertSpec creates a placeholder value to be
 * encrypted. It is only expected to be called when query_type is unset. The
 * output FLE2TextSearchInsertSpec is a BSON document of the form:
 * https://github.com/mongodb/mongo/blob/219e90bfad3c712c9642da29ee52229908f06bcd/src/mongo/crypto/fle_field_schema.idl#L689
 *
 * v is expect to be a BSON document of the form:
 * { "v": BSON value to encrypt }.
 *
 * Preconditions: out must be initialized by caller.
 */
bool mc_TextOpts_to_FLE2TextSearchInsertSpec(const mc_TextOpts_t *txo,
                                             const bson_t *v,
                                             bson_t *out,
                                             mongocrypt_status_t *status);

#endif // MC_TEXTOPTS_PRIVATE_H
