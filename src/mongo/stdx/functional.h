/**
 *    Copyright (C) 2014 10gen Inc.
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

#pragma once

// As of VS2013, the Windows STL still doesn't have an adequate implementation
// of std::function.
//
// See https://connect.microsoft.com/VisualStudio/feedback/details/768899/
// std-function-not-compiling-in-vs2012
//
// The bug is fixed in VS2015.
#if !defined(_MSC_VER) || (_MSC_VER > 1800)

#include <functional>

namespace mongo {
namespace stdx {

using ::std::bind;                             // NOLINT
using ::std::function;                         // NOLINT
namespace placeholders = ::std::placeholders;  // NOLINT

}  // namespace stdx
}  // namespace mongo

#else

#include <boost/bind.hpp>
#include <boost/function.hpp>

namespace mongo {
namespace stdx {

using boost::bind;      // NOLINT
using boost::function;  // NOLINT

namespace placeholders {
static boost::arg<1> _1;
static boost::arg<2> _2;
static boost::arg<3> _3;
static boost::arg<4> _4;
static boost::arg<5> _5;
static boost::arg<6> _6;
static boost::arg<7> _7;
static boost::arg<8> _8;
static boost::arg<9> _9;
}  // namespace placeholders

}  // namespace stdx
}  // namespace mongo

#endif
