// server_parameters_inline.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/util/stringutils.h"

namespace mongo {

// We create template specializations for each possible value type which is supported at runtime.
// The only value types which are supported at runtime are types which can be stored in
// AtomicWord<T> or AtomicProxy<T> which both have explicit load and store methods. The storage type
// for a value type is chosen by the server_parameter_storage_type type trait. Since there is no
// support for partial template specialization of member functions, we generate 4 (the Atomic types)
// x 2 (RuntimeOnly, StartupAndRuntime) implementations of append and set.
#define EXPORTED_ATOMIC_SERVER_PARAMETER_TYPE(VALUE_TYPE, PARAM_TYPE)        \
    template <>                                                              \
    inline void ExportedServerParameter<VALUE_TYPE, PARAM_TYPE>::append(     \
        OperationContext* txn, BSONObjBuilder& b, const std::string& name) { \
        b.append(name, _value->load());                                      \
    }                                                                        \
                                                                             \
    template <>                                                              \
    inline Status ExportedServerParameter<VALUE_TYPE, PARAM_TYPE>::set(      \
        const VALUE_TYPE& newValue) {                                        \
        Status v = validate(newValue);                                       \
        if (!v.isOK())                                                       \
            return v;                                                        \
                                                                             \
        _value->store(newValue);                                             \
        return Status::OK();                                                 \
    }

#define EXPORTED_ATOMIC_SERVER_PARAMETER(PARAM_TYPE)             \
    EXPORTED_ATOMIC_SERVER_PARAMETER_TYPE(bool, PARAM_TYPE)      \
    EXPORTED_ATOMIC_SERVER_PARAMETER_TYPE(int, PARAM_TYPE)       \
    EXPORTED_ATOMIC_SERVER_PARAMETER_TYPE(long long, PARAM_TYPE) \
    EXPORTED_ATOMIC_SERVER_PARAMETER_TYPE(double, PARAM_TYPE)

EXPORTED_ATOMIC_SERVER_PARAMETER(ServerParameterType::kRuntimeOnly);
EXPORTED_ATOMIC_SERVER_PARAMETER(ServerParameterType::kStartupAndRuntime);

template <typename T, ServerParameterType paramType>
inline Status ExportedServerParameter<T, paramType>::set(const BSONElement& newValueElement) {
    T newValue;

    if (!newValueElement.coerce(&newValue))
        return Status(ErrorCodes::BadValue, "can't set value");

    return set(newValue);
}

template <typename T, ServerParameterType paramType>
inline Status ExportedServerParameter<T, paramType>::set(const T& newValue) {
    Status v = validate(newValue);
    if (!v.isOK())
        return v;

    *_value = newValue;
    return Status::OK();
}

template <typename T, ServerParameterType paramType>
void ExportedServerParameter<T, paramType>::append(OperationContext* txn,
                                                   BSONObjBuilder& b,
                                                   const std::string& name) {
    b.append(name, *_value);
}

}  // namespace mongo
