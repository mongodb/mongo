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

#include "mongo/db/auth/user_name.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/util/assert_util.h"

namespace mongo {

UserName::UserName(StringData user, StringData dbname) {
    _fullName.resize(user.size() + dbname.size() + 1);
    std::string::iterator iter =
        std::copy(user.rawData(), user.rawData() + user.size(), _fullName.begin());
    *iter = '@';
    ++iter;
    iter = std::copy(dbname.rawData(), dbname.rawData() + dbname.size(), iter);
    dassert(iter == _fullName.end());
    _splitPoint = user.size();
}

/**
 * Don't change the logic of this function as it will break stable API version 1.
 */
StatusWith<UserName> UserName::parse(StringData userNameStr) {
    size_t splitPoint = userNameStr.find('.');

    if (splitPoint == std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      "username must contain a '.' separated database.user pair");
    }

    StringData userDBPortion = userNameStr.substr(0, splitPoint);
    StringData userNamePortion = userNameStr.substr(splitPoint + 1);

    return UserName(userNamePortion, userDBPortion);
}

/**
 * Don't change the logic of this function as it will break stable API version 1.
 */
UserName UserName::parseFromVariant(const stdx::variant<std::string, BSONObj>& helloUserName) {
    if (stdx::holds_alternative<std::string>(helloUserName)) {
        return uassertStatusOK(parse(stdx::get<std::string>(helloUserName)));
    }

    return parseFromBSONObj(stdx::get<BSONObj>(helloUserName));
}

/**
 * Don't change the logic of this function as it will break stable API version 1.
 */
UserName UserName::parseFromBSONObj(const BSONObj& obj) {
    std::bitset<2> usedFields;
    const auto kUserNameFieldName = AuthorizationManager::USER_NAME_FIELD_NAME;
    const auto kUserDbFieldName = AuthorizationManager::USER_DB_FIELD_NAME;
    const size_t kUserNameFieldBit = 0;
    const size_t kUserDbFieldBit = 1;
    StringData userName, userDb;

    for (const auto& element : obj) {
        const auto fieldName = element.fieldNameStringData();

        uassert(ErrorCodes::BadValue,
                str::stream() << "username contains an unknown field named: '" << fieldName,
                fieldName == kUserNameFieldName || fieldName == kUserDbFieldName);

        uassert(ErrorCodes::BadValue,
                str::stream() << "username must contain a string field named: " << fieldName,
                element.type() == String);

        if (fieldName == kUserNameFieldName) {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "username has more than one field named: "
                                  << kUserNameFieldName,
                    !usedFields[kUserNameFieldBit]);

            usedFields.set(kUserNameFieldBit);
            userName = element.valueStringData();
        } else if (fieldName == kUserDbFieldName) {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "username has more than one field named: " << kUserDbFieldName,
                    !usedFields[kUserDbFieldBit]);

            usedFields.set(kUserDbFieldBit);
            userDb = element.valueStringData();
        }
    }

    if (!usedFields[kUserNameFieldBit]) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "username must contain a field named: " << kUserNameFieldName);
    }

    if (!usedFields[kUserDbFieldBit]) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "username must contain a field named: " << kUserDbFieldName);
    }

    return UserName(userName, userDb);
}

UserName UserName::parseFromBSON(const BSONElement& elem) {
    if (elem.type() == String) {
        return uassertStatusOK(UserName::parse(elem.valueStringData()));
    } else if (elem.type() == Object) {
        const auto obj = elem.embeddedObject();
        return parseFromBSONObj(obj);
    } else {
        uasserted(ErrorCodes::BadValue, "username must be either a string or an object");
    }
}

void UserName::serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {
    BSONObjBuilder sub(bob->subobjStart(fieldName));
    appendToBSON(&sub);
}

void UserName::serializeToBSON(BSONArrayBuilder* bab) const {
    BSONObjBuilder sub(bab->subobjStart());
    appendToBSON(&sub);
}

/**
 * Don't change the logic of this function as it will break stable API version 1.
 */
void UserName::appendToBSON(BSONObjBuilder* bob) const {
    *bob << AuthorizationManager::USER_NAME_FIELD_NAME << getUser()
         << AuthorizationManager::USER_DB_FIELD_NAME << getDB();
}

/**
 * Don't change the logic of this function as it will break stable API version 1.
 */
BSONObj UserName::toBSON() const {
    BSONObjBuilder ret;
    appendToBSON(&ret);
    return ret.obj();
}

std::ostream& operator<<(std::ostream& os, const UserName& name) {
    return os << name.getFullName();
}

}  // namespace mongo
