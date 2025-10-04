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

#include "mc-dec128.h"
#include "mc-fle-blob-subtype-private.h"
#include "mc-fle2-payload-iev-private-v2.h"
#include "mc-fle2-tag-and-encrypted-metadata-block-private.h"
#include "mc-reader-private.h"
#include "mc-tokens-private.h"
#include "mc-writer-private.h"
#include <mongocrypt-util-private.h>
#include <stdint.h>

#define kMinServerEncryptedValueLen 17U // IV(16) + EncryptCTR(1byte)

#define CHECK_AND_RETURN(x)                                                                                            \
    if (!(x)) {                                                                                                        \
        return false;                                                                                                  \
    } else                                                                                                             \
        ((void)0)

mc_FLE2IndexedEncryptedValueV2_t *mc_FLE2IndexedEncryptedValueV2_new(void) {
    return bson_malloc0(sizeof(mc_FLE2IndexedEncryptedValueV2_t));
}

bson_type_t mc_FLE2IndexedEncryptedValueV2_get_bson_value_type(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                               mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (iev->type == kFLE2IEVTypeInitV2) {
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

    if (iev->type == kFLE2IEVTypeInitV2) {
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

    if (iev->type == kFLE2IEVTypeInitV2) {
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

    if (iev->metadata) {
        for (uint32_t i = 0; i < iev->edge_count; i++) {
            mc_FLE2TagAndEncryptedMetadataBlock_cleanup(&iev->metadata[i]);
        }

        // Metadata array is dynamically allocated
        bson_free(iev->metadata);
    }

    bson_free(iev);
}

uint32_t mc_FLE2IndexedEncryptedValueV2_get_edge_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                       mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge_count "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return 0;
    }

    if (!(iev->type == kFLE2IEVTypeRangeV2 || iev->type == kFLE2IEVTypeText)) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge_count must be called with type range or text");
        return 0;
    }

    return iev->edge_count;
}

bool mc_FLE2IndexedEncryptedValueV2_get_substr_tag_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                         uint32_t *count,
                                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(count);

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_substr_tag_count "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kFLE2IEVTypeText) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_substr_tag_count must be called with type text");
        return false;
    }

    *count = iev->substr_tag_count;
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_get_suffix_tag_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                         uint32_t *count,
                                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(count);

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_suffix_tag_count "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kFLE2IEVTypeText) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_suffix_tag_count must be called with type text");
        return false;
    }

    *count = iev->suffix_tag_count;
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_get_prefix_tag_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                         uint32_t *count,
                                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(count);

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_prefix_tag_count "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kFLE2IEVTypeText) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_prefix_tag_count must be called with type text");
        return false;
    }

    *count = (uint32_t)(iev->edge_count - iev->substr_tag_count - iev->suffix_tag_count - 1);
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_get_edge(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                             mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                             const uint32_t edge_index,
                                             mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(out);

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kFLE2IEVTypeRangeV2 && iev->type != kFLE2IEVTypeText) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge must be called with type range");
        return false;
    }

    if (edge_index >= iev->edge_count) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_edge must be called with index edge_index less "
                   "than edge count");
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

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_metadata "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kFLE2IEVTypeEqualityV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_metadata must be called with type equality");
        return false;
    }

    // Write edge into out struct
    *out = *iev->metadata;
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_get_exact_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                       mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                       mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(out);

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_exact_metadata "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kFLE2IEVTypeText) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_exact_metadata must be called with type text");
        return false;
    }

    // Write edge into out struct
    *out = iev->metadata[0];
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_get_substr_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                        mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                        const uint32_t block_index,
                                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(out);

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_substr_metadata "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kFLE2IEVTypeText) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_substr_metadata must be called with type text");
        return false;
    }

    if (block_index >= iev->substr_tag_count) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_substr_metadata must be called with index block_index less "
                   "than substr tag count");
        return false;
    }

    // Write edge into out struct
    *out = iev->metadata[block_index + 1 /* exact block */];
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_get_suffix_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                        mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                        const uint32_t block_index,
                                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(out);

    if (iev->type == kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_suffix_metadata "
                   "must be called after "
                   "mc_FLE2IndexedEncryptedValueV2_parse");
        return false;
    }

    if (iev->type != kFLE2IEVTypeText) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_suffix_metadata must be called with type text");
        return false;
    }

    if (block_index >= iev->suffix_tag_count) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_suffix_metadata must be called with index block_index less "
                   "than suffix tag count");
        return false;
    }

    // Write edge into out struct
    *out = iev->metadata[block_index + iev->substr_tag_count + 1 /* exact block */];
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_get_prefix_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                        mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                        const uint32_t block_index,
                                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(out);

    // We can skip the check for text type because get_prefix_tag_count does it for us.
    uint32_t prefix_tag_count;
    if (!mc_FLE2IndexedEncryptedValueV2_get_prefix_tag_count(iev, &prefix_tag_count, status)) {
        return false;
    }
    if (block_index >= prefix_tag_count) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_get_prefix_metadata must be called with index block_index less "
                   "than prefix tag count");
        return false;
    }

    // Write edge into out struct
    *out = iev->metadata[block_index + iev->suffix_tag_count + iev->substr_tag_count + 1 /* exact block */];
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

    if (iev->type != kFLE2IEVTypeInitV2) {
        CLIENT_ERR("mc_FLE2IndexedRangeEncryptedValueV2_parse must not be "
                   "called twice");
        return false;
    }

    mc_reader_t reader;
    mc_reader_init_from_buffer(&reader, buf, __func__);

    CHECK_AND_RETURN(mc_reader_read_u8(&reader, &iev->fle_blob_subtype, status));

    if (iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2) {
        iev->type = kFLE2IEVTypeEqualityV2;
    } else if (iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2) {
        iev->type = kFLE2IEVTypeRangeV2;
    } else if (iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedTextEncryptedValue) {
        iev->type = kFLE2IEVTypeText;
    } else {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_parse expected "
                   "fle_blob_subtype MC_SUBTYPE_FLE2Indexed(Equality|Range|Text)EncryptedValue[V2] got: %" PRIu8,
                   iev->fle_blob_subtype);
        return false;
    }

    /* Read S_KeyId. */
    CHECK_AND_RETURN(mc_reader_read_uuid_buffer(&reader, &iev->S_KeyId, status));

    /* Read original_bson_type. */
    CHECK_AND_RETURN(mc_reader_read_u8(&reader, &iev->bson_value_type, status));

    /* Read edge_count, substr_tag_count, suffix_tag_count */
    // Set equality edge_count to 1 as it doesn't technically exist but
    // there will be a singular metadata block
    // Set substr/suffix_tag_count to 0 for all types besides text
    iev->substr_tag_count = 0;
    iev->suffix_tag_count = 0;
    if (iev->type == kFLE2IEVTypeEqualityV2) {
        iev->edge_count = 1;
    } else {
        if (iev->type == kFLE2IEVTypeRangeV2) {
            uint8_t ec;
            CHECK_AND_RETURN(mc_reader_read_u8(&reader, &ec, status));
            if (ec == 0) {
                CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_parse edge count must not be 0 for type "
                           "range, but found edge count is 0.");
                return false;
            }
            iev->edge_count = (uint32_t)ec;
        } else if (iev->type == kFLE2IEVTypeText) {
            CHECK_AND_RETURN(mc_reader_read_u32(&reader, &iev->edge_count, status));
            CHECK_AND_RETURN(mc_reader_read_u32(&reader, &iev->substr_tag_count, status));
            CHECK_AND_RETURN(mc_reader_read_u32(&reader, &iev->suffix_tag_count, status));
            // Upconvert so that addition doesn't overflow
            if ((uint64_t)iev->edge_count < (uint64_t)iev->substr_tag_count + (uint64_t)iev->suffix_tag_count + 1) {
                CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_parse for text type expected edge count to be "
                           "at least substr_tag_count + suffix_count + 1, but: %" PRIu32 " < %" PRIu32 " + %" PRIu32
                           " + 1",
                           iev->edge_count,
                           iev->substr_tag_count,
                           iev->suffix_tag_count);
                return false;
            }
        }
    }

    // Maximum edge_count(4294967295) times kMetadataLen(96) fits easily without
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

    // Read each metadata element in buff
    for (uint32_t i = 0; i < iev->edge_count; i++) {
        _mongocrypt_buffer_t tmp_buf;

        CHECK_AND_RETURN(mc_reader_read_buffer(&reader, &tmp_buf, kMetadataLen, status));
        CHECK_AND_RETURN(mc_FLE2TagAndEncryptedMetadataBlock_parse(&iev->metadata[i], &tmp_buf, status));

        _mongocrypt_buffer_cleanup(&tmp_buf);
    }

    return true;
}

static inline uint32_t mc_FLE2IndexedEncryptedValueV2_serialized_length(const mc_FLE2IndexedEncryptedValueV2_t *iev) {
    // fle_blob_subtype: 1 byte
    // S_KeyId: UUID_LEN bytes
    // bson_value_type: 1 byte
    // if range: edge_count: 1 byte
    // if text: edge + tag counts: 12 bytes
    // ServerEncryptedValue: ServerEncryptedValue.len bytes
    // metadata: edge_count * kMetadataLen bytes
    return iev->ServerEncryptedValue.len + 1 + UUID_LEN + 1 + (iev->type == kFLE2IEVTypeRangeV2 ? 1 : 0)
         + (iev->type == kFLE2IEVTypeText ? 12 : 0) + iev->edge_count * kMetadataLen;
}

bool mc_FLE2IndexedEncryptedValueV2_serialize(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                              _mongocrypt_buffer_t *buf,
                                              mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(buf);

    if (iev->type != kFLE2IEVTypeRangeV2 && iev->type != kFLE2IEVTypeEqualityV2 && iev->type != kFLE2IEVTypeText) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_serialize must be called with type equality, range, or text");
        return false;
    }

    uint32_t expected_len = mc_FLE2IndexedEncryptedValueV2_serialized_length(iev);
    mc_writer_t writer;
    _mongocrypt_buffer_resize(buf, expected_len);
    mc_writer_init_from_buffer(&writer, buf, __func__);

    // Serialize fle_blob_subtype
    CHECK_AND_RETURN(mc_writer_write_u8(&writer, iev->fle_blob_subtype, status));

    // Serialize S_KeyId
    CHECK_AND_RETURN(mc_writer_write_uuid_buffer(&writer, &iev->S_KeyId, status));

    // Serialize bson_value_type
    CHECK_AND_RETURN(mc_writer_write_u8(&writer, iev->bson_value_type, status));

    if (iev->type == kFLE2IEVTypeRangeV2) {
        // Serialize edge_count (only serialized for types range and text)
        CHECK_AND_RETURN(mc_writer_write_u8(&writer, (uint8_t)iev->edge_count, status));
    } else if (iev->type == kFLE2IEVTypeText) {
        // Serialize substr/suffix_tag_count (only serialized for text)
        CHECK_AND_RETURN(mc_writer_write_u32(&writer, iev->edge_count, status));
        CHECK_AND_RETURN(mc_writer_write_u32(&writer, iev->substr_tag_count, status));
        CHECK_AND_RETURN(mc_writer_write_u32(&writer, iev->suffix_tag_count, status));
    }

    // Serialize encrypted value
    CHECK_AND_RETURN(
        mc_writer_write_buffer(&writer, &iev->ServerEncryptedValue, iev->ServerEncryptedValue.len, status));

    // Serialize metadata
    for (uint32_t i = 0; i < iev->edge_count; ++i) {
        _mongocrypt_buffer_t tmp_buf;
        _mongocrypt_buffer_init(&tmp_buf);

        CHECK_AND_RETURN(mc_FLE2TagAndEncryptedMetadataBlock_serialize(&iev->metadata[i], &tmp_buf, status));
        CHECK_AND_RETURN(mc_writer_write_buffer(&writer, &tmp_buf, kMetadataLen, status));

        _mongocrypt_buffer_cleanup(&tmp_buf);
    }

    return true;
}

static bool is_fle2_equality_indexed_supported_type(int bson_type) {
    switch (bson_type) {
    case BSON_TYPE_BINARY:
    case BSON_TYPE_CODE:
    case BSON_TYPE_REGEX:
    case BSON_TYPE_UTF8:

    case BSON_TYPE_INT32:
    case BSON_TYPE_INT64:
    case BSON_TYPE_BOOL:
    case BSON_TYPE_TIMESTAMP:
    case BSON_TYPE_DATE_TIME:
    case BSON_TYPE_OID:

    case BSON_TYPE_SYMBOL:
    case BSON_TYPE_DBPOINTER: return true;
    default: // All other defined types are non-deterministic or singletons.
        return false;
    }
}

static bool is_fle2_range_indexed_supported_type(int bson_type) {
    switch (bson_type) {
    case BSON_TYPE_INT32:
    case BSON_TYPE_INT64:
    case BSON_TYPE_DATE_TIME:
    case BSON_TYPE_DOUBLE:
#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT()
    case BSON_TYPE_DECIMAL128:
#endif
        return true;
    default: return false;
    }
}

static bool is_fle2_text_indexed_supported_type(int bson_type) {
    return bson_type == BSON_TYPE_UTF8;
}

#define CHECK(condition, msg)                                                                                          \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_validate failed: " msg);                                        \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool validate_for_equality(const mc_FLE2IndexedEncryptedValueV2_t *iev, mongocrypt_status_t *status) {
    CHECK(iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2,
          "fle_blob_subtype does not match type");
    CHECK(is_fle2_equality_indexed_supported_type(iev->bson_value_type), "bson_value_type is invalid");
    CHECK(iev->edge_count == 1, "edge_count must be 1 for equality");
    return true;
}

static bool validate_for_range(const mc_FLE2IndexedEncryptedValueV2_t *iev, mongocrypt_status_t *status) {
    CHECK(iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2, "fle_blob_subtype does not match type");
    CHECK(is_fle2_range_indexed_supported_type(iev->bson_value_type), "bson_value_type is invalid");
    if (iev->edge_count > (uint32_t)UINT8_MAX) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValueV2_validate failed: edge count for range encrypted value "
                   "must be less than max uint8_t. Got: %" PRIu32,
                   iev->edge_count);
    }
    return true;
}

static bool validate_for_text(const mc_FLE2IndexedEncryptedValueV2_t *iev, mongocrypt_status_t *status) {
    CHECK(iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedTextEncryptedValue, "fle_blob_subtype does not match type");
    CHECK(is_fle2_text_indexed_supported_type(iev->bson_value_type), "bson_value_type is invalid");
    CHECK((uint64_t)iev->edge_count >= (uint64_t)iev->substr_tag_count + (uint64_t)iev->suffix_tag_count + 1,
          "edge_count is smaller than substr_tag_count + suffix_tag_count + 1");
    return true;
}

bool mc_FLE2IndexedEncryptedValueV2_validate(const mc_FLE2IndexedEncryptedValueV2_t *iev, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    CHECK(iev->type == kFLE2IEVTypeEqualityV2 || iev->type == kFLE2IEVTypeRangeV2 || iev->type == kFLE2IEVTypeText,
          "type was init or unknown");

    if (iev->type == kFLE2IEVTypeEqualityV2) {
        validate_for_equality(iev, status);
    } else if (iev->type == kFLE2IEVTypeRangeV2) {
        validate_for_range(iev, status);
    } else {
        validate_for_text(iev, status);
    }

    if (!mongocrypt_status_ok(status)) {
        return false;
    }

    CHECK(iev->ServerEncryptedValue.len >= kMinServerEncryptedValueLen, "SEV.len is less than minimum");
    CHECK(iev->S_KeyId.len == UUID_LEN, "S_KeyId is not the correct length for a UUID");

    CHECK(!iev->ClientValueDecoded || iev->ClientEncryptedValueDecoded,
          "Found decrypted client value without encrypted client value");
    if (iev->ClientEncryptedValueDecoded) {
        const _mongocrypt_value_encryption_algorithm_t *fle2alg = _mcFLE2Algorithm();
        const uint32_t DecryptedServerEncryptedValueLen =
            fle2alg->get_plaintext_len(iev->ServerEncryptedValue.len, status);
        if (!mongocrypt_status_ok(status)) {
            return false;
        }
        CHECK(iev->DecryptedServerEncryptedValue.len == DecryptedServerEncryptedValueLen, "DSEV.len was unexpected");
        CHECK(iev->ClientEncryptedValue.len == iev->DecryptedServerEncryptedValue.len - UUID_LEN,
              "CEV.len was unexpected");
        CHECK(iev->K_KeyId.len == UUID_LEN, "K_KeyId is not the correct length for a UUID");
    }
    if (iev->ClientValueDecoded) {
        const _mongocrypt_value_encryption_algorithm_t *fle2v2aead = _mcFLE2v2AEADAlgorithm();
        const uint32_t ClientValueLen = fle2v2aead->get_plaintext_len(iev->ClientEncryptedValue.len, status);
        if (!mongocrypt_status_ok(status)) {
            return false;
        }
        CHECK(iev->ClientValue.len == ClientValueLen, "ClientValue.len was unexpected");
    }
    CHECK(iev->edge_count > 0, "edge_count must be at least 1");
    for (uint32_t i = 0; i < iev->edge_count; i++) {
        if (!mc_FLE2TagAndEncryptedMetadataBlock_validate(&iev->metadata[i], status)) {
            return false;
        }
    }
    return true;
}
