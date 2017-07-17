/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/service_liason.h"

#include "mongo/db/keys_collection_manager_zero.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/service_context.h"

namespace mongo {

namespace {

const int kSignatureSize = sizeof(UUID) + sizeof(OID);

SHA1Block computeSignature(const SignedLogicalSessionId* id, TimeProofService::Key key) {
    // Write the uuid and user id to a block for signing.
    char signatureBlock[kSignatureSize] = {0};
    DataRangeCursor cursor(signatureBlock, signatureBlock + kSignatureSize);
    auto res = cursor.writeAndAdvance<ConstDataRange>(id->getLsid().getId().toCDR());
    invariant(res.isOK());
    if (auto userId = id->getUserId()) {
        res = cursor.writeAndAdvance<ConstDataRange>(userId->toCDR());
        invariant(res.isOK());
    }

    // Compute the signature.
    return SHA1Block::computeHmac(
        key.data(), key.size(), reinterpret_cast<uint8_t*>(signatureBlock), kSignatureSize);
}

KeysCollectionManagerZero kKeysCollectionManagerZero{"HMAC"};

}  // namespace

ServiceLiason::~ServiceLiason() = default;

StatusWith<SignedLogicalSessionId> ServiceLiason::signLsid(OperationContext* opCtx,
                                                           LogicalSessionId* lsid,
                                                           boost::optional<OID> userId) {
    auto& keyManager = kKeysCollectionManagerZero;

    auto logicalTime = LogicalClock::get(_context())->getClusterTime();
    auto res = keyManager.getKeyForSigning(opCtx, logicalTime);
    if (!res.isOK()) {
        return res.getStatus();
    }

    SignedLogicalSessionId signedLsid;
    signedLsid.setUserId(std::move(userId));
    signedLsid.setLsid(*lsid);

    auto keyDoc = res.getValue();
    signedLsid.setKeyId(keyDoc.getKeyId());

    auto signature = computeSignature(&signedLsid, keyDoc.getKey());
    signedLsid.setSignature(std::move(signature));

    return signedLsid;
}

Status ServiceLiason::validateLsid(OperationContext* opCtx, const SignedLogicalSessionId& id) {
    auto& keyManager = kKeysCollectionManagerZero;

    // Attempt to get the correct key.
    auto logicalTime = LogicalClock::get(_context())->getClusterTime();
    auto res = keyManager.getKeyForValidation(opCtx, id.getKeyId(), logicalTime);
    if (!res.isOK()) {
        return res.getStatus();
    }

    // Re-compute the signature, and see that it matches.
    auto signature = computeSignature(&id, res.getValue().getKey());
    if (signature != id.getSignature()) {
        return {ErrorCodes::NoSuchSession, "Signature validation failed."};
    }

    return Status::OK();
}

}  // namespace mongo
