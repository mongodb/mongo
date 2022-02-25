/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/crypto/fle_key_vault_shell.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/shell/kms.h"

namespace mongo {

KeyMaterial FLEKeyVaultShellImpl::getKey(const UUID& uuid) {
    FindCommandRequest findCmd{_nss};
    findCmd.setFilter(BSON("_id" << uuid));
    findCmd.setReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern).toBSONInner());

    BSONObj dataKeyObj = _conn->findOne(std::move(findCmd));
    if (dataKeyObj.isEmpty()) {
        uasserted(ErrorCodes::BadValue, "Invalid keyID.");
    }

    auto keyStoreRecord = KeyStoreRecord::parse(IDLParserErrorContext("root"), dataKeyObj);
    if (dataKeyObj.hasField("version"_sd)) {
        uassert(ErrorCodes::BadValue,
                "Invalid version, must be either 1 or undefined",
                dataKeyObj.getIntField("version"_sd) == 1);
    }

    BSONElement elem = dataKeyObj.getField("keyMaterial"_sd);
    uassert(ErrorCodes::BadValue, "Invalid key.", elem.isBinData(BinDataType::BinDataGeneral));
    uassert(ErrorCodes::BadValue,
            "Invalid version, must be either 0 or undefined",
            keyStoreRecord.getVersion() == 0);

    auto dataKey = keyStoreRecord.getKeyMaterial();
    uassert(ErrorCodes::BadValue, "Invalid data key.", dataKey.length() != 0);

    std::unique_ptr<KMSService> kmsService = KMSServiceController::createFromDisk(
        _encryptionOptions.getKmsProviders().toBSON(), keyStoreRecord.getMasterKey());

    return kmsService->decrypt(dataKey, keyStoreRecord.getMasterKey());
}

}  // namespace mongo
