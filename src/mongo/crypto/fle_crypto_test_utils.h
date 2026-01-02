/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"

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

    static EncryptedFieldHelper makeUnindexed(StringData path, BSONType type, UUID indexKeyId);
    static EncryptedFieldHelper makeEquality(StringData path,
                                             BSONType type,
                                             UUID indexKeyId,
                                             boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makeRange(StringData path,
                                          BSONType type,
                                          UUID indexKeyId,
                                          boost::optional<Value> min = boost::none,
                                          boost::optional<Value> max = boost::none,
                                          boost::optional<int64_t> sparsity = boost::none,
                                          boost::optional<int32_t> trimFactor = boost::none,
                                          boost::optional<int32_t> precision = boost::none,
                                          boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makeSuffix(StringData path,
                                           BSONType type,
                                           UUID indexKeyId,
                                           int lb,
                                           int ub,
                                           bool caseSensitive = false,
                                           bool diacriticSensitive = false,
                                           boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makePrefix(StringData path,
                                           BSONType type,
                                           UUID indexKeyId,
                                           int lb,
                                           int ub,
                                           bool caseSensitive = false,
                                           bool diacriticSensitive = false,
                                           boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makeSubstring(StringData path,
                                              BSONType type,
                                              UUID indexKeyId,
                                              int lb,
                                              int ub,
                                              int mlen,
                                              bool caseSensitive = false,
                                              bool diacriticSensitive = false,
                                              boost::optional<int64_t> contention = boost::none);
    static EncryptedFieldHelper makePrefixSuffix(StringData path,
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
    EncryptedFieldHelper(Fle2AlgorithmInt alg, StringData path, UUID keyId, BSONType type);

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
