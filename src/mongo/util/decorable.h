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

/**
 * This header describes a mechanism for making "decorable" types.
 *
 * A decorable type is one to which various subsystems may attach subsystem-private data, so long as
 * they declare what that data will be before any instances of the decorable type are created.
 *
 * For example, suppose you had a class Client, representing on a server a network connection to a
 * client process.  Suppose that your server has an authentication module, that attaches data to the
 * client about authentication.  If class Client looks something like this:
 *
 * class Client : public Decorable<Client>{
 * ...
 * };
 *
 * Then the authentication module, before the first client object is created, calls
 *
 *     const auto authDataDescriptor = Client::declareDecoration<AuthenticationPrivateData>();
 *
 * And stores authDataDescriptor in a module-global variable,
 *
 * And later, when it has a Client object, client, and wants to get at the per-client
 * AuthenticationPrivateData object, it calls
 *
 *    authDataDescriptor(client)
 *
 * to get a reference to the AuthenticationPrivateData for that client object.
 *
 * With this approach, individual subsystems get to privately augment the client object via
 * declarations local to the subsystem, rather than in the global client header.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/util/decoration_container.h"
#include "mongo/util/decoration_registry.h"

namespace mongo {

template <typename D>
class Decorable {
    MONGO_DISALLOW_COPYING(Decorable);

public:
    template <typename T>
    class Decoration {
    public:
        Decoration() = delete;

        T& operator()(D& d) const {
            return static_cast<Decorable&>(d)._decorations.getDecoration(_raw);
        }

        T& operator()(D* d) const {
            return (*this)(*d);
        }

        const T& operator()(const D& d) const {
            return static_cast<const Decorable&>(d)._decorations.getDecoration(_raw);
        }

        const T& operator()(const D* d) const {
            return (*this)(*d);
        }

    private:
        friend class Decorable;

        explicit Decoration(DecorationContainer::DecorationDescriptorWithType<T> raw)
            : _raw(std::move(raw)) {}

        DecorationContainer::DecorationDescriptorWithType<T> _raw;
    };

    template <typename T>
    static Decoration<T> declareDecoration() {
        return Decoration<T>(getRegistry()->declareDecoration<T>());
    }

protected:
    Decorable() : _decorations(getRegistry()) {}
    ~Decorable() = default;

private:
    static DecorationRegistry* getRegistry() {
        static DecorationRegistry* theRegistry = new DecorationRegistry();
        return theRegistry;
    }

    DecorationContainer _decorations;
};

}  // namespace mongo
