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

#include <boost/optional.hpp>

#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo::fle {

using DerivedToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator;
using TwiceDerived = FLETwiceDerivedTokenGenerator;

// The algorithm for constructing a list of tags matching an equality predicate on an encrypted
// field is as follows:
//
// (1) Query ESC to obtain the counter value (n) after the most recent insert.
// (2) Set the initial candidate set of tags to 1..n inclusive.
//      (a) We know there have been (n) inserts into the collection for this field/value pair,
//          however there could have been deletes, so this represents a superset of the true set of
//          tags.
// (3) Query ECC for a null document.
//      (a) A null document means there has been at least one compaction. Therefore the deletes that
//          were present in ECC before the compaction, have now been reflected in ESC.
//      (b) No null document means there has not been a compaction. Therefore we have to check all
//          the tags from 1..n to see if they have been deleted.
// (4) Return the surviving set of tags from 1..n (encrypted).
//
// Given:
//      s: ESCDerivedFromDataToken
//      c: ECCDerivedFromDataToken
//      d: EDCDerivedFromDataToken
// Do:
//      n = ESC::emuBinary(s)
//      tags = [1..n]
//      pos = ECC::nullDocument(c) ? ECC::nullDocument(c).position : 1
//      LOOP:
//          doc = ECC::getDocument(c, pos)
//          if doc {
//              tags = tags \ [doc.start..doc.end]
//          } else {
//              break
//          }
//          pos++
//      return [EDC::encrypt(i) | i in tags]
void readTagsWithContention(const FLEStateCollectionReader& esc,
                            const FLEStateCollectionReader& ecc,
                            ESCDerivedFromDataToken s,
                            ECCDerivedFromDataToken c,
                            EDCDerivedFromDataToken d,
                            uint64_t cf,
                            std::vector<PrfBlock>& binaryTags) {

    auto escTok = DerivedToken::generateESCDerivedFromDataTokenAndContentionFactorToken(s, cf);
    auto escTag = TwiceDerived::generateESCTwiceDerivedTagToken(escTok);
    auto escVal = TwiceDerived::generateESCTwiceDerivedValueToken(escTok);

    auto eccTok = DerivedToken::generateECCDerivedFromDataTokenAndContentionFactorToken(c, cf);
    auto eccTag = TwiceDerived::generateECCTwiceDerivedTagToken(eccTok);
    auto eccVal = TwiceDerived::generateECCTwiceDerivedValueToken(eccTok);

    auto edcTok = DerivedToken::generateEDCDerivedFromDataTokenAndContentionFactorToken(d, cf);
    auto edcTag = TwiceDerived::generateEDCTwiceDerivedToken(edcTok);

    // (1) Query ESC for the counter value after the most recent insert.
    //     0 => 0 inserts for this field value pair.
    //     n => n inserts for this field value pair.
    //     none => compaction => query ESC for null document to find # of inserts.
    auto insertCounter = ESCCollection::emuBinary(esc, escTag, escVal);
    if (insertCounter && insertCounter.get() == 0) {
        return;
    }

    stdx::unordered_set<int64_t> tags;

    auto numInserts = insertCounter
        ? uassertStatusOK(
              ESCCollection::decryptDocument(
                  escVal, esc.getById(ESCCollection::generateId(escTag, insertCounter))))
              .count
        : uassertStatusOK(ESCCollection::decryptNullDocument(
                              escVal, esc.getById(ESCCollection::generateId(escTag, boost::none))))
              .count;

    // (2) Set the initial set of tags to 1..n inclusive - a superset of the true tag set.
    for (uint64_t i = 1; i <= numInserts; i++) {
        tags.insert(i);
    }

    // (3) Query ECC for a null document.
    auto eccNullDoc = ecc.getById(ECCCollection::generateId(eccTag, boost::none));
    auto pos = eccNullDoc.isEmpty()
        ? 1
        : uassertStatusOK(ECCCollection::decryptNullDocument(eccVal, eccNullDoc)).position + 2;

    // (3) Search ECC for deleted tag(counter) values.
    while (true) {
        auto eccObj = ecc.getById(ECCCollection::generateId(eccTag, pos));
        if (eccObj.isEmpty()) {
            break;
        }
        auto eccDoc = uassertStatusOK(ECCCollection::decryptDocument(eccVal, eccObj));
        // Compaction placeholders only present for positive contention factors (cm).
        if (eccDoc.valueType == ECCValueType::kCompactionPlaceholder) {
            break;
        }
        for (uint64_t i = eccDoc.start; i <= eccDoc.end; i++) {
            tags.erase(i);
        }
        pos++;
    }

    for (auto counter : tags) {
        // (4) Derive binary tag values (encrypted) from the set of counter values (tags).
        binaryTags.emplace_back(EDCServerCollection::generateTag(edcTag, counter));
    }
}

// A positive contention factor (cm) means we must run the above algorithm (cm) times.
std::vector<PrfBlock> readTags(const FLEStateCollectionReader& esc,
                               const FLEStateCollectionReader& ecc,
                               ESCDerivedFromDataToken s,
                               ECCDerivedFromDataToken c,
                               EDCDerivedFromDataToken d,
                               boost::optional<int64_t> cm) {
    std::vector<PrfBlock> binaryTags;
    if (!cm || cm.get() == 0) {
        readTagsWithContention(esc, ecc, s, c, d, 0, binaryTags);
        return binaryTags;
    }
    for (auto i = 1; i <= cm.get(); i++) {
        readTagsWithContention(esc, ecc, s, c, d, i, binaryTags);
    }
    return binaryTags;
}
}  // namespace mongo::fle
