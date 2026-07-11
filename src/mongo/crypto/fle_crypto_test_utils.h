// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * Convenience wrapper class around the EncryptedField IDL type used for easy creation
 * of EncryptedField objects for tailored for various queryable encryption query types.
 *
 * This also provides the helper function for converting a given BSONElement to a intent-to-encrypt
 * placeholder using the stored EncryptedField context.
 */
class EncryptedFieldHelper {
public:
    EncryptedFieldHelper(EncryptedField fieldSchema);

    static EncryptedFieldHelper makeUnindexed(std::string_view path,
                                              BSONType type,
                                              UUID indexKeyId);
    static EncryptedFieldHelper makeEquality(std::string_view path,
                                             BSONType type,
                                             UUID indexKeyId,
                                             boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makeRange(std::string_view path,
                                          BSONType type,
                                          UUID indexKeyId,
                                          boost::optional<Value> min = boost::none,
                                          boost::optional<Value> max = boost::none,
                                          boost::optional<int64_t> sparsity = boost::none,
                                          boost::optional<int32_t> trimFactor = boost::none,
                                          boost::optional<int32_t> precision = boost::none,
                                          boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makeSuffix(std::string_view path,
                                           BSONType type,
                                           UUID indexKeyId,
                                           int lb,
                                           int ub,
                                           bool caseSensitive = false,
                                           bool diacriticSensitive = false,
                                           boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makePrefix(std::string_view path,
                                           BSONType type,
                                           UUID indexKeyId,
                                           int lb,
                                           int ub,
                                           bool caseSensitive = false,
                                           bool diacriticSensitive = false,
                                           boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makeSubstring(std::string_view path,
                                              BSONType type,
                                              UUID indexKeyId,
                                              int lb,
                                              int ub,
                                              int mlen,
                                              bool caseSensitive = false,
                                              bool diacriticSensitive = false,
                                              boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makePrefixSuffix(std::string_view path,
                                                 BSONType type,
                                                 UUID indexKeyId,
                                                 int lb,
                                                 int ub,
                                                 bool caseSensitive = false,
                                                 bool diacriticSensitive = false,
                                                 boost::optional<int64_t> contention = boost::none);

    std::vector<char> generatePlaceholder(BSONElement value,
                                          Fle2PlaceholderType op,
                                          boost::optional<UUID> userKeyId = boost::none) const;

    const EncryptedField& getEncryptedField() const {
        return _ef;
    }

private:
    EncryptedFieldHelper(Fle2AlgorithmInt alg, std::string_view path, UUID keyId, BSONType type);

    Fle2AlgorithmInt _algorithm;
    EncryptedField _ef;
    std::vector<QueryTypeConfig> _queries;
    int64_t _contentionMax = 0;
};


/**
 * Helper class used in FLE2 unit tests to perform the client-side workflow in a
 * queryable encryption operation.
 *
 * The workflow typically goes as follows:
 *
 *    ClientSideEncryptor client(ns);
 *    EncryptedField field;
 *    ... // Set the encrypted field options in field here...
 *    client.addEncryptedField(field);
 *
 *    BSONObj inputDoc; // document containing the field with plaintext
 *
 *    // Replace encrypted field plaintext values with intent-to-encrypt placeholders.
 *    BSONObj markedDoc = client.replaceWithPlaceholders(inputDoc, Fle2PlaceholderType::kInsert);
 *    // Perform the client-side encryption. Returns the document sent to the server.
 *    BSONObj encryptedDoc = client.encryptPlaceholders(inputDoc, markedDoc, keyVault);
 *
 * This class keeps a table of encrypted field paths and their respective encryption options. This
 * can be configured either by constructing from an EncryptedFieldConfig, or by adding individual
 * encrypted fields to the table using addEncryptedField().
 *
 * When mocking client-side query analysis using replaceWithPlaceholders(), it uses this
 * table to identify which fields to replace, and then uses the respective encryption options
 * (e.g. the query type, algorithm, keyId, etc) to create the placeholder.
 */
class ClientSideEncryptor {
public:
    explicit ClientSideEncryptor(NamespaceString edcNs) : _edcNs(edcNs) {}
    ClientSideEncryptor(NamespaceString edcNs, const EncryptedFieldConfig& efc);

    void addEncryptedField(EncryptedFieldHelper efk);
    void addEncryptedField(EncryptedField ef);

    BSONObj replaceWithPlaceholders(BSONObj inputDoc,
                                    Fle2PlaceholderType op,
                                    boost::optional<UUID> userKeyId = boost::none) const;
    BSONObj encryptPlaceholders(BSONObj unmarkedDoc, BSONObj markedDoc, FLEKeyVault& kv) const;

    EncryptedFieldConfig efc() const;
    NamespaceString ns() const {
        return _edcNs;
    }

private:
    BSONObj _replaceWithPlaceholders(BSONObj inputDoc,
                                     Fle2PlaceholderType op,
                                     FieldRef& path,
                                     boost::optional<UUID> userKeyId = boost::none) const;

    std::map<std::string, std::shared_ptr<EncryptedFieldHelper>> _fields;
    NamespaceString _edcNs;
    boost::optional<std::int32_t> _strEncodeVersion;
};

}  // namespace mongo
