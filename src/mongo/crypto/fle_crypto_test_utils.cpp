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

#include "mongo/crypto/fle_crypto_test_utils.h"

#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/encryption_fields_validation.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"

namespace mongo {

EncryptedFieldHelper::EncryptedFieldHelper(EncryptedField fieldSchema) {
    validateEncryptedField(&fieldSchema);
    _ef = std::move(fieldSchema);
    _algorithm = Fle2AlgorithmInt::kUnindexed;

    visitQueryTypeConfigs(_ef, [this](const EncryptedField& field, const QueryTypeConfig& qtc) {
        _contentionMax = qtc.getContention();
        switch (qtc.getQueryType()) {
            case QueryTypeEnum::Equality:
                _algorithm = Fle2AlgorithmInt::kEquality;
                break;
            case QueryTypeEnum::RangePreviewDeprecated:
            case QueryTypeEnum::Range:
                _algorithm = Fle2AlgorithmInt::kRange;
                break;
            case QueryTypeEnum::SubstringPreview:
            case QueryTypeEnum::SuffixPreview:
            case QueryTypeEnum::PrefixPreview:
                _algorithm = Fle2AlgorithmInt::kTextSearch;
                break;
            default:
                break;
        };
        _queries.push_back(qtc);
        if (_algorithm == Fle2AlgorithmInt::kRange) {
            setRangeDefaults(
                typeFromName(field.getBsonType().value()), field.getPath(), &_queries.front());
        }
        return false;  // always false to visit all QueryTypeConfigs
    });
}

EncryptedFieldHelper::EncryptedFieldHelper(Fle2AlgorithmInt alg,
                                           StringData path,
                                           UUID keyId,
                                           BSONType type)
    : _algorithm(alg), _ef(keyId, std::string(path)) {
    _ef.setBsonType(std::string(typeName(type)));
}

// Mock for query analysis
std::vector<char> EncryptedFieldHelper::generatePlaceholder(BSONElement value,
                                                            Fle2PlaceholderType operation,
                                                            boost::optional<UUID> userKeyId) const {
    FLE2EncryptionPlaceholder ep;

    ep.setType(operation);
    ep.setAlgorithm(_algorithm);
    ep.setIndexKeyId(_ef.getKeyId());
    ep.setUserKeyId(userKeyId.value_or(_ef.getKeyId()));

    BSONObj valueBSON;
    if (_algorithm == Fle2AlgorithmInt::kRange) {
        auto& qtc = _queries.front();
        auto bounds = BSON_ARRAY(qtc.getMin().value() << qtc.getMax().value());

        if (operation == Fle2PlaceholderType::kInsert) {
            auto spec = FLE2RangeInsertSpec(IDLAnyType(value));
            spec.setMinBound(boost::optional<IDLAnyType>(bounds[0]));
            spec.setMaxBound(boost::optional<IDLAnyType>(bounds[1]));
            spec.setPrecision(qtc.getPrecision());
            spec.setTrimFactor(qtc.getTrimFactor());
            valueBSON = BSON("s" << spec.toBSON());
        } else {
            FLE2RangeFindSpec spec;
            FLE2RangeFindSpecEdgesInfo edgesInfo;
            edgesInfo.setLowerBound(value);
            edgesInfo.setLbIncluded(true);
            edgesInfo.setUpperBound(value);
            edgesInfo.setUbIncluded(true);
            edgesInfo.setPrecision(qtc.getPrecision());
            edgesInfo.setTrimFactor(qtc.getTrimFactor());
            edgesInfo.setIndexMin(bounds[0]);
            edgesInfo.setIndexMax(bounds[1]);
            spec.setEdgesInfo(edgesInfo);
            spec.setPayloadId(1234);
            spec.setFirstOperator(Fle2RangeOperator::kGte);
            spec.setSecondOperator(Fle2RangeOperator::kLte);
            valueBSON = BSON("s" << spec.toBSON());
        }
        ep.setValue(IDLAnyType(valueBSON.firstElement()));
        ep.setSparsity(qtc.getSparsity());
    } else if (_algorithm == Fle2AlgorithmInt::kTextSearch) {
        bool caseFold = !_queries.front().getCaseSensitive().value();
        bool diacriticFold = !_queries.front().getDiacriticSensitive().value();
        FLE2TextSearchInsertSpec spec(value.String(), caseFold, diacriticFold);

        for (auto& qtc : _queries) {
            auto lb = qtc.getStrMinQueryLength().value();
            auto ub = qtc.getStrMaxQueryLength().value();
            switch (qtc.getQueryType()) {
                case QueryTypeEnum::SubstringPreview: {
                    auto mlen = qtc.getStrMaxLength().value();
                    spec.setSubstringSpec(FLE2SubstringInsertSpec(mlen, ub, lb));
                    break;
                }
                case QueryTypeEnum::SuffixPreview: {
                    spec.setSuffixSpec(FLE2SuffixInsertSpec(ub, lb));
                    break;
                }
                case QueryTypeEnum::PrefixPreview: {
                    spec.setPrefixSpec(FLE2PrefixInsertSpec(ub, lb));
                    break;
                }
                default:
                    break;
            }
        }
        valueBSON = BSON("" << spec.toBSON());
        ep.setValue(IDLAnyType(valueBSON.firstElement()));
    } else {
        ep.setValue(IDLAnyType(value));
    }
    ep.setMaxContentionCounter(_contentionMax);

    BSONObj obj = ep.toBSON();
    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}


EncryptedFieldHelper EncryptedFieldHelper::makeUnindexed(StringData path,
                                                         BSONType type,
                                                         UUID indexKeyId) {
    return EncryptedFieldHelper(Fle2AlgorithmInt::kUnindexed, path, indexKeyId, type);
}

EncryptedFieldHelper EncryptedFieldHelper::makeEquality(StringData path,
                                                        BSONType type,
                                                        UUID indexKeyId,
                                                        boost::optional<int64_t> contention) {
    EncryptedFieldHelper res(Fle2AlgorithmInt::kEquality, path, indexKeyId, type);
    res._queries.emplace_back(QueryTypeEnum::Equality);
    auto& qtc = res._queries.back();
    res._contentionMax = contention.value_or(qtc.getContention());
    qtc.setContention(res._contentionMax);

    res._ef.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{res._queries});
    return res;
}

EncryptedFieldHelper EncryptedFieldHelper::makeRange(StringData path,
                                                     BSONType type,
                                                     UUID indexKeyId,
                                                     boost::optional<Value> min,
                                                     boost::optional<Value> max,
                                                     boost::optional<int64_t> sparsity,
                                                     boost::optional<int32_t> trimFactor,
                                                     boost::optional<int32_t> precision,
                                                     boost::optional<int64_t> contention) {
    EncryptedFieldHelper res(Fle2AlgorithmInt::kRange, path, indexKeyId, type);
    res._queries.emplace_back(QueryTypeEnum::Range);
    auto& qtc = res._queries.back();
    res._contentionMax = contention.value_or(qtc.getContention());
    qtc.setContention(res._contentionMax);

    qtc.setMin(min);
    qtc.setMax(max);
    qtc.setSparsity(sparsity);
    qtc.setTrimFactor(trimFactor);
    qtc.setPrecision(precision);
    setRangeDefaults(type, path, &qtc);

    res._ef.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{res._queries});

    return res;
}

EncryptedFieldHelper EncryptedFieldHelper::makeSuffix(StringData path,
                                                      BSONType type,
                                                      UUID indexKeyId,
                                                      int lb,
                                                      int ub,
                                                      bool caseSensitive,
                                                      bool diacriticSensitive,
                                                      boost::optional<int64_t> contention) {
    EncryptedFieldHelper res(Fle2AlgorithmInt::kTextSearch, path, indexKeyId, type);
    res._queries.emplace_back(QueryTypeEnum::SuffixPreview);
    auto& qtc = res._queries.back();
    res._contentionMax = contention.value_or(qtc.getContention());
    qtc.setContention(res._contentionMax);

    qtc.setStrMaxQueryLength(ub);
    qtc.setStrMinQueryLength(lb);
    qtc.setDiacriticSensitive(diacriticSensitive);
    qtc.setCaseSensitive(caseSensitive);
    res._ef.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{res._queries});
    return res;
}

EncryptedFieldHelper EncryptedFieldHelper::makePrefix(StringData path,
                                                      BSONType type,
                                                      UUID indexKeyId,
                                                      int lb,
                                                      int ub,
                                                      bool caseSensitive,
                                                      bool diacriticSensitive,
                                                      boost::optional<int64_t> contention) {
    auto res =
        makeSuffix(path, type, indexKeyId, lb, ub, caseSensitive, diacriticSensitive, contention);
    res._queries.back().setQueryType(QueryTypeEnum::PrefixPreview);
    res._ef.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{res._queries});
    return res;
}

EncryptedFieldHelper EncryptedFieldHelper::makeSubstring(StringData path,
                                                         BSONType type,
                                                         UUID indexKeyId,
                                                         int lb,
                                                         int ub,
                                                         int mlen,
                                                         bool caseSensitive,
                                                         bool diacriticSensitive,
                                                         boost::optional<int64_t> contention) {
    auto res =
        makeSuffix(path, type, indexKeyId, lb, ub, caseSensitive, diacriticSensitive, contention);
    res._queries.back().setQueryType(QueryTypeEnum::SubstringPreview);
    res._queries.back().setStrMaxLength(mlen);
    res._ef.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{res._queries});
    return res;
}

EncryptedFieldHelper EncryptedFieldHelper::makePrefixSuffix(StringData path,
                                                            BSONType type,
                                                            UUID indexKeyId,
                                                            int lb,
                                                            int ub,
                                                            bool caseSensitive,
                                                            bool diacriticSensitive,
                                                            boost::optional<int64_t> contention) {
    auto res =
        makeSuffix(path, type, indexKeyId, lb, ub, caseSensitive, diacriticSensitive, contention);
    auto res2 =
        makePrefix(path, type, indexKeyId, lb, ub, caseSensitive, diacriticSensitive, contention);
    res._queries.push_back(res2._queries.back());
    res._ef.setQueries(std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>{res._queries});
    return res;
}


ClientSideEncryptor::ClientSideEncryptor(NamespaceString edcNs, const EncryptedFieldConfig& efc)
    : _edcNs(edcNs) {
    validateEncryptedFieldConfig(&efc);
    auto& fields = efc.getFields();
    for (auto& field : fields) {
        addEncryptedField(field);
    }
    _strEncodeVersion = efc.getStrEncodeVersion();
}

void ClientSideEncryptor::addEncryptedField(EncryptedFieldHelper field) {
    auto path = std::string(field.getEncryptedField().getPath());
    dassert(!_fields.contains(path));
    _fields[path] = std::make_shared<EncryptedFieldHelper>(field);
}

void ClientSideEncryptor::addEncryptedField(EncryptedField fieldSchema) {
    addEncryptedField(EncryptedFieldHelper{fieldSchema});
}

BSONObj ClientSideEncryptor::replaceWithPlaceholders(BSONObj inputDoc,
                                                     Fle2PlaceholderType op,
                                                     boost::optional<UUID> userKeyId) const {
    FieldRef path;
    return _replaceWithPlaceholders(inputDoc, op, path, userKeyId);
}

BSONObj ClientSideEncryptor::_replaceWithPlaceholders(BSONObj inputDoc,
                                                      Fle2PlaceholderType op,
                                                      FieldRef& path,
                                                      boost::optional<UUID> userKeyId) const {
    BSONObjBuilder out;
    BSONObjIterator itr(inputDoc);

    while (itr.more()) {
        BSONElement elem = itr.next();
        FieldRef::FieldRefTempAppend appender(path, elem.fieldNameStringData());

        if (auto fieldItr = _fields.find(std::string(path.dottedField()));
            fieldItr != _fields.end()) {
            auto data = fieldItr->second->generatePlaceholder(elem, op, userKeyId);
            out.appendBinData(
                elem.fieldNameStringData(), data.size(), BinDataType::Encrypt, data.data());
            continue;
        }

        if (elem.type() == BSONType::object) {
            out.append(elem.fieldNameStringData(), _replaceWithPlaceholders(elem.Obj(), op, path));
        } else if (elem.type() == BSONType::array) {
            out.appendArray(elem.fieldNameStringData(), elem.Obj());
        } else {
            out.append(elem);
        }
    }
    return out.obj();
}

BSONObj ClientSideEncryptor::encryptPlaceholders(BSONObj unmarkedDoc,
                                                 BSONObj markedDoc,
                                                 FLEKeyVault& keyVault) const {
    // Wrap the document in an insert command, so libmongocrypt can transform
    // the placeholders. The placeholders will be transformed regardless of whether
    // they are for insert/update or find.
    auto origCmd = write_ops::InsertCommandRequest(_edcNs, {unmarkedDoc}).toBSON();
    auto cryptdResponse = [&]() {
        BSONObjBuilder bob;
        bob.append("hasEncryptionPlaceholders", true);
        bob.append("schemaRequiresEncryption", true);
        bob.append("result", write_ops::InsertCommandRequest(_edcNs, {markedDoc}).toBSON());
        return bob.obj();
    }();
    auto finalCmd =
        FLEClientCrypto::transformPlaceholders(origCmd,
                                               cryptdResponse,
                                               BSON(_edcNs.toString_forTest() << efc().toBSON()),
                                               &keyVault,
                                               _edcNs.db_forTest())
            .addField(BSON("$db" << _edcNs.db_forTest()).firstElement());
    return write_ops::InsertCommandRequest::parse(finalCmd, IDLParserContext("finalCmd"))
        .getDocuments()
        .front()
        .getOwned();
}

EncryptedFieldConfig ClientSideEncryptor::efc() const {
    EncryptedFieldConfig efc;
    efc.setEscCollection(fmt::format("enxcol_.{}.esc", _edcNs.coll()));
    efc.setEcocCollection(fmt::format("enxcol_.{}.ecoc", _edcNs.coll()));

    std::vector<EncryptedField> efs;
    std::transform(_fields.begin(), _fields.end(), std::back_inserter(efs), [](const auto& pair) {
        return pair.second->getEncryptedField();
    });
    efc.setFields(std::move(efs));
    efc.setStrEncodeVersion(_strEncodeVersion);
    return efc;
}

}  // namespace mongo
