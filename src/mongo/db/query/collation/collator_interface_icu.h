/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/collation/collator_interface.h"

#include <memory>

namespace icu {
class Collator;
}  // namespace icu

namespace mongo {

/**
 * An implementation of the CollatorInterface which is backed by the implementation of collations
 * from the ICU library.
 */
class CollatorInterfaceICU final : public CollatorInterface {
public:
    CollatorInterfaceICU(Collation spec, std::unique_ptr<icu::Collator> collator);

    std::unique_ptr<CollatorInterface> clone() const final;
    std::shared_ptr<CollatorInterface> cloneShared() const final;

    int compare(StringData left, StringData right) const final;

    ComparisonKey getComparisonKey(StringData stringData) const final;

private:
    // The ICU implementation of the collator to which we delegate interesting work. Const methods
    // on the ICU collator are expected to be thread-safe.
    const std::unique_ptr<icu::Collator> _collator;
};

}  // namespace mongo
