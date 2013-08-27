/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/auth/user_name.h"

#include <algorithm>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    UserName::UserName(const StringData& user, const StringData& dbname) {
        _fullName.resize(user.size() + dbname.size() + 1);
        std::string::iterator iter = std::copy(user.rawData(),
                                               user.rawData() + user.size(),
                                               _fullName.begin());
        *iter = '@';
        ++iter;
        iter = std::copy(dbname.rawData(), dbname.rawData() + dbname.size(), iter);
        dassert(iter == _fullName.end());
        _splitPoint = user.size();
    }

    std::ostream& operator<<(std::ostream& os, const UserName& name) {
        return os << name.getFullName();
    }

}  // namespace mongo
