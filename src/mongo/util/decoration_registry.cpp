/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/util/decoration_registry.h"

namespace mongo {

DecorationContainer::DecorationDescriptor DecorationRegistry::declareDecoration(
    const size_t sizeBytes,
    const size_t alignBytes,
    const DecorationConstructorFn constructor,
    const DecorationDestructorFn destructor) {
    const size_t misalignment = _totalSizeBytes % alignBytes;
    if (misalignment) {
        _totalSizeBytes += alignBytes - misalignment;
    }
    DecorationContainer::DecorationDescriptor result(_totalSizeBytes);
    _decorationInfo.push_back(DecorationInfo(result, constructor, destructor));
    _totalSizeBytes += sizeBytes;
    return result;
}

void DecorationRegistry::construct(DecorationContainer* decorable) const {
    auto iter = _decorationInfo.cbegin();
    try {
        for (; iter != _decorationInfo.cend(); ++iter) {
            iter->constructor(decorable->getDecoration(iter->descriptor));
        }
    } catch (...) {
        try {
            while (iter != _decorationInfo.cbegin()) {
                --iter;
                iter->destructor(decorable->getDecoration(iter->descriptor));
            }
        } catch (...) {
            std::terminate();
        }
        throw;
    }
}

void DecorationRegistry::destruct(DecorationContainer* decorable) const {
    try {
        for (DecorationInfoVector::const_reverse_iterator iter = _decorationInfo.rbegin(),
                                                          end = _decorationInfo.rend();
             iter != end;
             ++iter) {
            iter->destructor(decorable->getDecoration(iter->descriptor));
        }
    } catch (...) {
        std::terminate();
    }
}

DecorationRegistry::DecorationInfo::DecorationInfo(
    DecorationContainer::DecorationDescriptor inDescriptor,
    DecorationConstructorFn inConstructor,
    DecorationDestructorFn inDestructor)
    : descriptor(std::move(inDescriptor)),
      constructor(std::move(inConstructor)),
      destructor(std::move(inDestructor)) {}

}  // namespace mongo
