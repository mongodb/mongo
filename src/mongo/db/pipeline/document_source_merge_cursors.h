/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/client/connpool.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/pipeline/document_source.h"

namespace mongo {

class DocumentSourceMergeCursors : public DocumentSource {
public:
    struct CursorDescriptor {
        CursorDescriptor(ConnectionString connectionString, std::string ns, CursorId cursorId)
            : connectionString(std::move(connectionString)),
              ns(std::move(ns)),
              cursorId(cursorId) {}

        ConnectionString connectionString;
        std::string ns;
        CursorId cursorId;
    };

    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    InitialSourceType getInitialSourceType() const final {
        return InitialSourceType::kInitialSource;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSource> create(
        std::vector<CursorDescriptor> cursorDescriptors,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /** Returns non-owning pointers to cursors managed by this stage.
     *  Call this instead of getNext() if you want access to the raw streams.
     *  This method should only be called at most once.
     */
    std::vector<DBClientCursor*> getCursors();

    /**
     * Returns the next object from the cursor, throwing an appropriate exception if the cursor
     * reported an error. This is a better form of DBClientCursor::nextSafe.
     */
    static Document nextSafeFrom(DBClientCursor* cursor);

protected:
    void doDispose() final;

private:
    struct CursorAndConnection {
        CursorAndConnection(const CursorDescriptor& cursorDescriptor);
        ScopedDbConnection connection;
        DBClientCursor cursor;
    };

    // using list to enable removing arbitrary elements
    typedef std::list<std::shared_ptr<CursorAndConnection>> Cursors;

    DocumentSourceMergeCursors(std::vector<CursorDescriptor> cursorDescriptors,
                               const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    // Converts _cursorDescriptors into active _cursors.
    void start();

    // This is the description of cursors to merge.
    const std::vector<CursorDescriptor> _cursorDescriptors;

    // These are the actual cursors we are merging. Created lazily.
    Cursors _cursors;
    Cursors::iterator _currentCursor;

    bool _unstarted;
};

}  // namespace mongo
