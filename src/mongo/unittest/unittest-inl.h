/**
 *    Copyright (C) 2008 10gen Inc.
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
 */

#pragma once

namespace mongo {
    namespace unittest {

        template <typename T>
        Test::RegistrationAgent<T>::RegistrationAgent(const std::string& suiteName,
                                                      const std::string& testName) {
            Suite::getSuite(suiteName)->add<T>(testName);
        }

        template<typename A, typename B>
        std::string ComparisonAssertion::getComparisonFailureMessage(const std::string &theOperator,
                                                                     const A &a, const B &b) {
            std::ostringstream os;
            os << "Expected " << _aexp << " " << theOperator << " " << _bexp
               << " (" << a << " " << theOperator << " " << b << ")";
            return os.str();
        }

    }  // namespace mongo
}  // namespace unittest
