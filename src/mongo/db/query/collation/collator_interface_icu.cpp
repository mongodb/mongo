/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/query/collation/collator_interface_icu.h"

#include "mongo/util/assert_util.h"

namespace mongo {

CollatorInterfaceICU::CollatorInterfaceICU(CollationSpec spec,
                                           std::unique_ptr<icu::Collator> collator)
    : CollatorInterface(std::move(spec)), _collator(std::move(collator)) {}

int CollatorInterfaceICU::compare(StringData left, StringData right) {
    // TODO: What happens if 'status' is a failure code? In what circumstances could this happen?
    UErrorCode status = U_ZERO_ERROR;
    auto compareResult = _collator->compare(icu::UnicodeString(left.rawData(), left.size()),
                                            icu::UnicodeString(right.rawData(), right.size()),
                                            status);
    invariant(U_SUCCESS(status));

    return compareResult;
}

}  // namespace mongo
