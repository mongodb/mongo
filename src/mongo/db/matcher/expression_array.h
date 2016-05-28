// expression_array.h

/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

class ArrayMatchingMatchExpression : public MatchExpression {
public:
    ArrayMatchingMatchExpression(MatchType matchType) : MatchExpression(matchType) {}
    virtual ~ArrayMatchingMatchExpression() {}

    Status setPath(StringData path);

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details) const;

    /**
     * @param e - has to be an array.  calls matchesArray with e as an array
     */
    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual bool matchesArray(const BSONObj& anArray, MatchDetails* details) const = 0;

    bool equivalent(const MatchExpression* other) const;

    const StringData path() const {
        return _path;
    }

private:
    StringData _path;
    ElementPath _elementPath;
};

class ElemMatchObjectMatchExpression : public ArrayMatchingMatchExpression {
public:
    ElemMatchObjectMatchExpression() : ArrayMatchingMatchExpression(ELEM_MATCH_OBJECT) {}
    Status init(StringData path, MatchExpression* sub);

    bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ElemMatchObjectMatchExpression> e =
            stdx::make_unique<ElemMatchObjectMatchExpression>();
        e->init(path(), _sub->shallowClone().release());
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual size_t numChildren() const {
        return 1;
    }

    virtual MatchExpression* getChild(size_t i) const {
        return _sub.get();
    }

private:
    std::unique_ptr<MatchExpression> _sub;
};

class ElemMatchValueMatchExpression : public ArrayMatchingMatchExpression {
public:
    ElemMatchValueMatchExpression() : ArrayMatchingMatchExpression(ELEM_MATCH_VALUE) {}
    virtual ~ElemMatchValueMatchExpression();

    Status init(StringData path);
    Status init(StringData path, MatchExpression* sub);
    void add(MatchExpression* sub);

    bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ElemMatchValueMatchExpression> e =
            stdx::make_unique<ElemMatchValueMatchExpression>();
        e->init(path());
        for (size_t i = 0; i < _subs.size(); ++i) {
            e->add(_subs[i]->shallowClone().release());
        }
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual std::vector<MatchExpression*>* getChildVector() {
        return &_subs;
    }

    virtual size_t numChildren() const {
        return _subs.size();
    }

    virtual MatchExpression* getChild(size_t i) const {
        return _subs[i];
    }

private:
    bool _arrayElementMatchesAll(const BSONElement& e) const;

    std::vector<MatchExpression*> _subs;
};

class SizeMatchExpression : public ArrayMatchingMatchExpression {
public:
    SizeMatchExpression() : ArrayMatchingMatchExpression(SIZE) {}
    Status init(StringData path, int size);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<SizeMatchExpression> e = stdx::make_unique<SizeMatchExpression>();
        e->init(path(), _size);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual bool matchesArray(const BSONObj& anArray, MatchDetails* details) const;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    int getData() const {
        return _size;
    }

private:
    int _size;  // >= 0 real, < 0, nothing will match
};
}
