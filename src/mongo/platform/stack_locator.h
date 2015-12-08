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

#include <boost/optional.hpp>
#include <cstddef>

namespace mongo {

/**
 *  Provides access to the current stack bounds and remaining
 *  available stack space.
 *
 *  To use one, create it on the stack, like this:
 *
 *  // Construct a new locator
 *  const StackLocator locator;
 *
 *  // Get the start of the stack
 *  auto b = locator.begin();
 *
 *  // Get the end of the stack
 *  auto e = locator.end();
 *
 *  // Get the remaining space after 'locator' on the stack.
 *  auto avail = locator.available();
 */
class StackLocator {
public:
    /**
     * Constructs a new StackLocator. The locator must have automatic
     * storage duration or the behavior is undefined.
     */
    StackLocator();

    /**
     *  Returns the address of the beginning of the stack, or nullptr
     *  if this cannot be done. Beginning here means those addresses
     *  that represent values of automatic duration found earlier in
     *  the call chain. Returns nullptr if the beginning of the stack
     *  could not be found.
     */
    void* begin() const {
        return _begin;
    }

    /**
     *  Returns the address of the end of the stack, or nullptr if
     *  this cannot be done. End here means those addresses that
     *  represent values of automatic duration allocated deeper in the
     *  call chain. Returns nullptr if the end of the stack could not
     *  be found.
     */
    void* end() const {
        return _end;
    }

    /**
     *  Returns the apparent size of the stack. Returns a disengaged
     *  optional if the size of the stack could not be determined.
     */
    boost::optional<size_t> size() const;

    /**
     *  Returns the remaining stack available after the location of
     *  this StackLocator. Obviously, the StackLocator must have been
     *  constructed on the stack. Calling 'available' on a heap
     *  allocated StackAllocator will have undefined behavior. Returns
     *  a disengaged optional if the remaining stack cannot be
     *  determined.
     */
    boost::optional<std::size_t> available() const;

private:
    void* _begin = nullptr;
    void* _end = nullptr;
};

}  // namespace mongo
