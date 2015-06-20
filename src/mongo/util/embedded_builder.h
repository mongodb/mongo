// embedded_builder.h

/*    Copyright 2009 10gen Inc.
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

#pragma once


namespace mongo {

// utility class for assembling hierarchical objects
class EmbeddedBuilder {
public:
    EmbeddedBuilder(BSONObjBuilder* b) {
        _builders.push_back(std::make_pair("", b));
    }
    // It is assumed that the calls to prepareContext will be made with the 'name'
    // parameter in lex ascending order.
    void prepareContext(std::string& name) {
        int i = 1, n = _builders.size();
        while (
            i < n && name.substr(0, _builders[i].first.length()) == _builders[i].first &&
            (name[_builders[i].first.length()] == '.' || name[_builders[i].first.length()] == 0)) {
            name = name.substr(_builders[i].first.length() + 1);
            ++i;
        }
        for (int j = n - 1; j >= i; --j) {
            popBuilder();
        }
        for (std::string next = splitDot(name); !next.empty(); next = splitDot(name)) {
            addBuilder(next);
        }
    }
    void appendAs(const BSONElement& e, std::string name) {
        if (e.type() == Object &&
            e.valuesize() == 5) {  // empty object -- this way we can add to it later
            std::string dummyName = name + ".foo";
            prepareContext(dummyName);
            return;
        }
        prepareContext(name);
        back()->appendAs(e, name);
    }
    BufBuilder& subarrayStartAs(std::string name) {
        prepareContext(name);
        return back()->subarrayStart(name);
    }
    void done() {
        while (!_builderStorage.empty())
            popBuilder();
    }

    static std::string splitDot(std::string& str) {
        size_t pos = str.find('.');
        if (pos == std::string::npos)
            return "";
        std::string ret = str.substr(0, pos);
        str = str.substr(pos + 1);
        return ret;
    }

private:
    void addBuilder(const std::string& name) {
        std::shared_ptr<BSONObjBuilder> newBuilder(new BSONObjBuilder(back()->subobjStart(name)));
        _builders.push_back(std::make_pair(name, newBuilder.get()));
        _builderStorage.push_back(newBuilder);
    }
    void popBuilder() {
        back()->done();
        _builders.pop_back();
        _builderStorage.pop_back();
    }

    BSONObjBuilder* back() {
        return _builders.back().second;
    }

    std::vector<std::pair<std::string, BSONObjBuilder*>> _builders;
    std::vector<std::shared_ptr<BSONObjBuilder>> _builderStorage;
};

}  // namespace mongo
