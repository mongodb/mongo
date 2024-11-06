/*
 * Copyright 2022-present MongoDB, Inc.
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

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-private.h"

#include "mc-fle-blob-subtype-private.h"
#include "mc-fle2-payload-iev-private-v2.h"
#include "mc-fle2-tag-and-encrypted-metadata-block-private.h"
#include "mc-reader-private.h"
#include "mc-tokens-private.h"
#include "mc-writer-private.h"
#include <stdint.h>

typedef enum {
    kTypeInit,
    kTypeEquality,
    kTypeRange,
} _mc_fle2_iev_v2_type;

struct _mc_FLE2IndexedEncryptedValueV2_t {
    // Raw payload values
    uint8_t fle_blob_subtype;
    uint8_t bson_value_type;
    uint8_t edge_count;
    _mongocrypt_buffer_t S_KeyId;
    _mongocrypt_buffer_t ServerEncryptedValue;

    // Decode State
    _mc_fle2_iev_v2_type type;
    bool ClientEncryptedValueDecoded;
    bool ClientValueDecoded;

    // Populated during _add_S_Key
    // DecryptedServerEncryptedValue := DecryptCTR(S_Key, ServerEncryptedValue)
    _mongocrypt_buffer_t DecryptedServerEncryptedValue;

    // Views on DecryptedServerEncryptedValue (DSEV)
    _mongocrypt_buffer_t K_KeyId;              // First 16 octets, UUID
    _mongocrypt_buffer_t ClientEncryptedValue; // Remainder of DSEV

    // Populated during _add_K_Key
    // ClientValue := DecryptCBCAEAD(K_Key, ClientEncryptedValue, AD=K_KeyId)
    _mongocrypt_buffer_t ClientValue;

    mc_FLE2TagAndEncryptedMetadataBlock_t *metadata;
};

#define kMetadataLen 96U                // encCount(32) + tag(32) + encZeros(32)
#define kMinServerEncryptedValueLen 17U // IV(16) + EncryptCTR(1byte)
#define kMinSEVAndMetadataLen (kMinServerEncryptedValueLen + kMetadataLen)

#define CHECK_AND_RETURN(x)                                                                                            \
    if (!(x)) {                                                                                                        \
        return false;                                                                                                  \
    }

mc_FLE2IndexedEncryptedValueV2_t *mc_FLE2IndexedEncryptedValueV2_new(void) {
    return bson_malloc0(sizeof(mc_FLE2IndexedEncryptedValueV2_t));
}

bson_type_t mc_FLE2IndexedEncryptedValueV2_get_bson_value_type(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                               mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (iev->type == kTypeInit) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_bson_value_type "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return BSON_TYPE_EOD;
    }

    return (bson_type_t)iev->bson_value_type;
}

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValueV2_get_S_KeyId(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                                       mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (iev->type == kTypeInit) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_S_KeyId "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return NULL;
    }

    return &iev->S_KeyId;
}

bool mc_FLE2IndexedEncryptedValueV2_add_S_Key(_mongocrypt_crypto_t *crypto,
                                              mc_FLE2IndexedEncryptedValueV2_t *iev,
                                              const _mongocrypt_buffer_t *S_Key,
                                              mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(S_Key);
    BSON_ASSERT_PARAM(status);

    if (iev->type == kTypeInit) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_add_S_Key must "
                   "be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->ClientEncryptedValueDecoded) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_add_S_Key must "
                   "not be called twice");
        return false;
    }

    if (S_Key->len != MONGOCRYPT_KEY_LEN) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_add_S_Key expected "
                   "S_Key to be %d bytes, got: %" PRIu32,
                   MONGOCRYPT_KEY_LEN,
                   S_Key->len);
        return false;
    }

    /* Get the TokenKey from the last 32 bytes of S_Key */
    _mongocrypt_buffer_t TokenKey;
    if (!_mongocrypt_buffer_from_subrange(&TokenKey,
                                          S_Key,
                                          S_Key->len - MONGOCRYPT_TOKEN_KEY_LEN,
                                          MONGOCRYPT_TOKEN_KEY_LEN)) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_add_S_Key unable to "
                   "parse TokenKey from S_Key");
        return false;
    }

    /* Use TokenKey to create ServerDataEncryptionLevel1Token and decrypt
     * ServerEncryptedValue into ClientEncryptedValue */
    mc_ServerDataEncryptionLevel1Token_t *token = mc_ServerDataEncryptionLevel1Token_new(crypto, &TokenKey, status);
    if (!token) {
        return false;
    }

    bool ret = false;
    const _mongocrypt_value_encryption_algorithm_t *fle2alg = _mcFLE2Algorithm();
    const uint32_t DecryptedServerEncryptedValueLen = fle2alg->get_plaintext_len(iev->ServerEncryptedValue.len, status);
    if (!mongocrypt_status_ok(status)) {
        goto fail;
    }
    if (DecryptedServerEncryptedValueLen <= UUID_LEN) {
        CLIENT_ERR("Invalid ServerEncryptedValue length, got %" PRIu32 ", expected more than %d",
                   DecryptedServerEncryptedValueLen,
                   UUID_LEN);
        goto fail;
    }
    _mongocrypt_buffer_resize(&iev->DecryptedServerEncryptedValue, DecryptedServerEncryptedValueLen);
    uint32_t bytes_written = 0;
    if (!fle2alg->do_decrypt(crypto,
                             NULL /* aad */,
                             mc_ServerDataEncryptionLevel1Token_get(token),
                             &iev->ServerEncryptedValue,
                             &iev->DecryptedServerEncryptedValue,
                             &bytes_written,
                             status)) {
        goto fail;
    }
    BSON_ASSERT(bytes_written == DecryptedServerEncryptedValueLen);
    if (!_mongocrypt_buffer_from_subrange(&iev->K_KeyId, &iev->DecryptedServerEncryptedValue, 0, UUID_LEN)) {
        CLIENT_ERR("Error creating K_KeyId subrange from DecryptedServerEncryptedValue");
        goto fail;
    }
    iev->K_KeyId.subtype = BSON_SUBTYPE_UUID;

    BSON_ASSERT(iev->DecryptedServerEncryptedValue.len > UUID_LEN);
    if (!_mongocrypt_buffer_from_subrange(&iev->ClientEncryptedValue,
                                          &iev->DecryptedServerEncryptedValue,
                                          UUID_LEN,
                                          iev->DecryptedServerEncryptedValue.len - UUID_LEN)) {
        CLIENT_ERR("Error creating ClientEncryptedValue subrange from "
                   "DecryptedServerEncryptedValue");
        goto fail;
    }

    iev->ClientEncryptedValueDecoded = true;
    ret = true;
fail:
    mc_ServerDataEncryptionLevel1Token_destroy(token);

    return ret;
}

const _mongocrypt_buffer_t *
mc_FLE2IndexedEncryptedValueV2_get_ClientEncryptedValue(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (!iev->ClientEncryptedValueDecoded) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_"
                   "ClientEncryptedValue must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_add_S_Key");
        return NULL;
    }

    return &iev->ClientEncryptedValue;
}

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValueV2_get_K_KeyId(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                                       mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (!iev->ClientEncryptedValueDecoded) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_K_KeyID "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_add_S_Key");
        return NULL;
    }

    return &iev->K_KeyId;
}

bool mc_FLE2IndexedEncryptedValueV2_add_K_Key(_mongocrypt_crypto_t *crypto,
                                              mc_FLE2IndexedEncryptedValueV2_t *iev,
                                              const _mongocrypt_buffer_t *K_Key,
                                              mongocrypt_status_t *status) {
    const _mongocrypt_value_encryption_algorithm_t *fle2v2aead = _mcFLE2v2AEADAlgorithm();

    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(K_Key);
    BSON_ASSERT_PARAM(status);

    if (!iev->ClientEncryptedValueDecoded) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_add_K_Key must be "
                   "called after "
                   "mc_FLE2IndexedEncryptedValueV2_add_S_Key");
        return false;
    }

    if (iev->ClientValueDecoded) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_add_K_Key must not "
                   "be called twice");
        return false;
    }

    /* Attempt to decrypt ClientEncryptedValue */
    const uint32_t ClientValueLen = fle2v2aead->get_plaintext_len(iev->ClientEncryptedValue.len, status);
    if (!mongocrypt_status_ok(status)) {
        return false;
    }

    _mongocrypt_buffer_t clientValue;
    _mongocrypt_buffer_init_size(&clientValue, ClientValueLen);
    uint32_t bytes_written = 0;
    if (!fle2v2aead->do_decrypt(crypto,
                                &iev->K_KeyId,
                                K_Key,
                                &iev->ClientEncryptedValue,
                                &clientValue,
                                &bytes_written,
                                status)) {
        _mongocrypt_buffer_cleanup(&clientValue);
        return false;
    }
    BSON_ASSERT(bytes_written > 0);
    BSON_ASSERT(bytes_written <= ClientValueLen);
    _mongocrypt_buffer_steal(&iev->ClientValue, &clientValue);
    iev->ClientValue.len = bytes_written;
    iev->ClientValueDecoded = true;

    return true;
}

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValueV2_get_ClientValue(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                                           mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (!iev->ClientValueDecoded) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_ClientValue must "
                   "be called after "
                   "mc_FLE2IndexedEncryptedValueV2_add_K_Key");
        return NULL;
    }

    return &iev->ClientValue;
}

void mc_FLE2IndexedEncryptedValueV2_destroy(mc_FLE2IndexedEncryptedValueV2_t *iev) {
    if (!iev) {
        return;
    }

    _mongocrypt_buffer_cleanup(&iev->ClientValue);
    _mongocrypt_buffer_cleanup(&iev->DecryptedServerEncryptedValue);
    _mongocrypt_buffer_cleanup(&iev->ServerEncryptedValue);
    _mongocrypt_buffer_cleanup(&iev->S_KeyId);

    for (int i = 0; i < iev->edge_count; i++) {
        mc_FLE2TagAndEncryptedMetadataBlock_cleanup(&iev->metadata[i]);
    }

    // Metadata array is dynamically allocated
    bson_free(iev->metadata);

    bson_free(iev);
}

uint8_t mc_FLE2IndexedEncryptedValueV2_get_edge_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                      mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (iev->type == kTypeInit) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge_count "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return 0;
    }

    if (iev->type != kTypeRange) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge_count must be called with type range");
        return 0;
    }

    return iev->edge_count;
}

bool mc_FLE2IndexedEncryptedValueV2_get_edge(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                             mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                             const uint8_t edge_index,
                                             mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(out);

    if (iev->type == kTypeInit) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kTypeRange) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge must be called with type range");
        return false;
    }

    if (edge_index >= iev->edge_count) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge must be called with index edge_index less than edge count");
        return false;
    }

    // Write edge into out struct
    *out = iev->metadata[edge_index];
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_get_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                 mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                 mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(out);

    if (iev->type == kTypeInit) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_metadata "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kTypeEquality) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_metadata must be called with type equality");
        return false;
    }

    // Write edge into out struct
    *out = *iev->metadata;
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_parse(mc_FLE2IndexedEncryptedValueV2_t *iev,
                                          const _mongocrypt_buffer_t *buf,
                                          mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(buf);

    if ((buf->data == NULL) || (buf->len == 0)) {
        CLIENT_ERR("Empty buffer passed to mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kTypeInit) {
        CLIENT_ERR("mc_FLE2IndexedRangeEncryptedValueV2_parse must not be "
                   "called twice");
        return false;
    }

    mc_reader_t reader;
    mc_reader_init_from_buffer(&reader, buf, __FUNCTION__);

    CHECK_AND_RETURN(mc_reader_read_u8(&reader, &iev->fle_blob_subtype, status));

    if (iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2) {
        iev->type = kTypeEquality;
    } else if (iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2) {
        iev->type = kTypeRange;
    } else {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_parse expected "
                   "fle_blob_subtype MC_SUBTYPE_FLE2Indexed(Equality|Range)EncryptedValueV2 got: %" PRIu8,
                   iev->fle_blob_subtype);
        return false;
    }

    /* Read S_KeyId. */
    CHECK_AND_RETURN(mc_reader_read_uuid_buffer(&reader, &iev->S_KeyId, status));

    /* Read original_bson_type. */
    CHECK_AND_RETURN(mc_reader_read_u8(&reader, &iev->bson_value_type, status));

    /* Read edge_count */
    // Set equality edge_count to 1 as it doesn't technically exist but
    // there will be a singular metadata block
    if (iev->type == kTypeEquality) {
        iev->edge_count = 1;
    } else {
        CHECK_AND_RETURN(mc_reader_read_u8(&reader, &iev->edge_count, status));
    }

    // Maximum edge_count(255) times kMetadataLen(96) fits easily without
    // overflow.
    const uint64_t metadata_len = iev->edge_count * kMetadataLen;

    /* Read ServerEncryptedValue. */
    const uint64_t min_required_len = kMinServerEncryptedValueLen + metadata_len;
    const uint64_t SEV_and_metadata_len = mc_reader_get_remaining_length(&reader);
    if (SEV_and_metadata_len < min_required_len) {
        CLIENT_ERR("Invalid payload size %" PRIu64 ", smaller than minimum length %" PRIu64,
                   SEV_and_metadata_len,
                   min_required_len);
        return false;
    }
    const uint64_t SEV_len = SEV_and_metadata_len - metadata_len;
    CHECK_AND_RETURN(mc_reader_read_buffer(&reader, &iev->ServerEncryptedValue, SEV_len, status));

    iev->metadata = (mc_FLE2TagAndEncryptedMetadataBlock_t *)bson_malloc0(
        iev->edge_count * sizeof(mc_FLE2TagAndEncryptedMetadataBlock_t));

    // Read each metadata element
    for (uint8_t i = 0; i < iev->edge_count; i++) {
        _mongocrypt_buffer_t tmp_buf;

        CHECK_AND_RETURN(mc_reader_read_buffer(&reader, &tmp_buf, kMetadataLen, status));
        CHECK_AND_RETURN(mc_FLE2TagAndEncryptedMetadataBlock_parse(&iev->metadata[i], &tmp_buf, status));

        _mongocrypt_buffer_cleanup(&tmp_buf);
    }

    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_serialize(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                              _mongocrypt_buffer_t *buf,
                                              mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(buf);

    if (iev->type != kTypeRange && iev->type != kTypeEquality) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_serialize must be called with type equality or range");
        return false;
    }

    mc_writer_t writer;
    mc_writer_init_from_buffer(&writer, buf, __FUNCTION__);

    // Serialize fle_blob_subtype
    CHECK_AND_RETURN(mc_writer_write_u8(&writer, iev->fle_blob_subtype, status));

    // Serialize S_KeyId
    CHECK_AND_RETURN(mc_writer_write_uuid_buffer(&writer, &iev->S_KeyId, status));

    // Serialize bson_value_type
    CHECK_AND_RETURN(mc_writer_write_u8(&writer, iev->bson_value_type, status));

    // Serialize edge_count (only serialized for type range)
    if (iev->type == kTypeRange) {
        CHECK_AND_RETURN(mc_writer_write_u8(&writer, iev->edge_count, status));
    }

    // Serialize encrypted value
    CHECK_AND_RETURN(
        mc_writer_write_buffer(&writer, &iev->ServerEncryptedValue, iev->ServerEncryptedValue.len, status));

    // Serialize metadata
    for (int i = 0; i < iev->edge_count; ++i) {
        _mongocrypt_buffer_t tmp_buf;
        _mongocrypt_buffer_init_size(&tmp_buf, kMetadataLen);

        CHECK_AND_RETURN(mc_FLE2TagAndEncryptedMetadataBlock_serialize(&iev->metadata[i], &tmp_buf, status));
        CHECK_AND_RETURN(mc_writer_write_buffer(&writer, &tmp_buf, kMetadataLen, status));

        _mongocrypt_buffer_cleanup(&tmp_buf);
    }

    return true;
}