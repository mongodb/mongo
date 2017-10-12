/**
 *    Copyright 2017 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/error_codes.h"

#include "mongo/base/static_assert.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

MONGO_STATIC_ASSERT(sizeof(ErrorCodes::Error) == sizeof(int));

std::string ErrorCodes::errorString(Error err) {
    switch (err) {
        //#for $ec in $codes
        case $ec.name:
            return "$ec.name";
        //#end for
        default:
            return mongoutils::str::stream() << "Location" << int(err);
    }
}

ErrorCodes::Error ErrorCodes::fromString(StringData name) {
    //#for $ec in $codes
    if (name == "$ec.name"_sd)
        return $ec.name;
    //#end for
    return UnknownError;
}

std::ostream& operator<<(std::ostream& stream, ErrorCodes::Error code) {
    return stream << ErrorCodes::errorString(code);
}

//#for $cat in $categories
bool ErrorCodes::is${cat.name}(Error err) {
    switch (err) {
        //#for $code in $cat.codes
        case $code:
            return true;
        //#end for
        default:
            return false;
    }
}
//#end for

void error_details::throwExceptionForStatus(const Status& status) {
    /**
     * This type is used for all exceptions that don't have a more specific type. It is defined
     * locally in this function to prevent anyone from catching it specifically separately from
     * AssertionException.
     */
    class NonspecificAssertionException final : public AssertionException {
    public:
        using AssertionException::AssertionException;

    private:
        void defineOnlyInFinalSubclassToPreventSlicing() final {}
    };

    switch (status.code()) {
        //#for $ec in $codes
        case ErrorCodes::$ec.name:
            throw ExceptionFor<ErrorCodes::$ec.name>(status);
        //#end for
        default:
            throw NonspecificAssertionException(status);
    }
}

}  // namespace mongo
