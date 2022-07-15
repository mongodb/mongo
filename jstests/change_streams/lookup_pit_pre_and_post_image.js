// Tests that the point-in-time pre- and post-images are loaded correctly in $changeStream running
// with different arguments for collections with 'changeStreamPreAndPostImages' being enabled.
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

const testDB = db.getSiblingDB(jsTestName());
const collName = "test";

const originalDoc = {
    _id: 1,
    x: 1
};
const updatedDoc = {
    _id: 1,
    x: 3
};
const updatedDoc2 = {
    _id: 1,
    x: 5
};
const replacedDoc = {
    _id: 1,
    z: 1
};

// Tests the change stream point-in-time pre-/post-images behaviour with different change stream
// options.
function preAndPostImageTest({
    changeStreamOptions = {},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled = {},
    expectedOnUpdateImages = {},
    expectedOnReplaceImages = {},
    expectedOnDeleteImages = {},
} = {}) {
    // Confirms that the change event document does not contain any internal-only fields.
    function assertChangeStreamInternalFieldsNotPresent(changeStreamDoc) {
        assert(!changeStreamDoc.hasOwnProperty("preImageId"), changeStreamDoc);
        assert(!changeStreamDoc.hasOwnProperty("updateModification"), changeStreamDoc);

        if (!changeStreamOptions.hasOwnProperty("fullDocumentBeforeChange")) {
            assert(!changeStreamDoc.hasOwnProperty("fullDocumentBeforeChange"), changeStreamDoc);
        }

        if (!changeStreamOptions.hasOwnProperty("fullDocument") &&
            changeStreamDoc.operationType == "update") {
            assert(!changeStreamDoc.hasOwnProperty("fullDocument"), changeStreamDoc);
        }
    }

    const coll = assertDropAndRecreateCollection(testDB, collName);

    // Open a change stream with the specified test options.
    let changeStreamCursor = coll.watch([], Object.assign({}, changeStreamOptions));
    let changeStreamDoc = null;

    // Perform an insert.
    assert.commandWorked(coll.insert(originalDoc));
    assert.soon(() => changeStreamCursor.hasNext());
    changeStreamDoc = changeStreamCursor.next();
    assert.eq(changeStreamDoc.operationType, 'insert');
    assertChangeStreamInternalFieldsNotPresent(changeStreamDoc);

    // Perform an update modification.
    assert.commandWorked(coll.update(originalDoc, {$inc: {x: 2}}));

    // Change stream should throw an exception while trying to fetch the next document if
    // pre-/post-image is required.
    const shouldThrow = changeStreamOptions.fullDocument === 'required' ||
        changeStreamOptions.fullDocumentBeforeChange === 'required';
    if (shouldThrow) {
        try {
            assert.soon(() => changeStreamCursor.hasNext());
            assert(false, `Unexpected result from cursor: ${tojson(changeStreamCursor.next())}`);
        } catch (error) {
            assert.eq(error.code,
                      ErrorCodes.NoMatchingDocument,
                      `Caught unexpected error: ${tojson(error)}`);
        }

        // Reopen the failed change stream.
        changeStreamCursor = coll.watch([], Object.assign({}, changeStreamOptions));
    } else {
        assert.soon(() => changeStreamCursor.hasNext());
        changeStreamDoc = changeStreamCursor.next();
        assert.eq(changeStreamDoc.fullDocumentBeforeChange,
                  expectedOnUpdateImagesWithChangeStreamPreImagesDisabled.preImage);
        assert.eq(changeStreamDoc.fullDocument,
                  expectedOnUpdateImagesWithChangeStreamPreImagesDisabled.postImage);
        assertChangeStreamInternalFieldsNotPresent(changeStreamDoc);
    }

    // Enable changeStreamPreAndPostImages for pre-images recording.
    assert.commandWorked(
        testDB.runCommand({collMod: collName, changeStreamPreAndPostImages: {enabled: true}}));

    // Perform an update modification.
    assert.commandWorked(coll.update(updatedDoc, {$inc: {x: 2}}));

    // The next change stream event should contain the expected pre- and post-images.
    assert.soon(() => changeStreamCursor.hasNext());
    changeStreamDoc = changeStreamCursor.next();
    assert.eq(changeStreamDoc.fullDocumentBeforeChange, expectedOnUpdateImages.preImage);
    assert.eq(changeStreamDoc.fullDocument, expectedOnUpdateImages.postImage);
    assertChangeStreamInternalFieldsNotPresent(changeStreamDoc);

    // Perform a full-document replacement.
    assert.commandWorked(coll.update(updatedDoc2, replacedDoc));

    // The next change stream event should contain the expected pre- and post-images.
    assert.soon(() => changeStreamCursor.hasNext());
    changeStreamDoc = changeStreamCursor.next();
    assert.eq(changeStreamDoc.fullDocumentBeforeChange, expectedOnReplaceImages.preImage);
    assert.eq(changeStreamDoc.fullDocument, expectedOnReplaceImages.postImage);
    assert.eq(changeStreamDoc.operationType, "replace");
    assertChangeStreamInternalFieldsNotPresent(changeStreamDoc);

    // Perform a document removal.
    assert.commandWorked(coll.remove(replacedDoc));

    // The next change stream event should contain the expected pre-image.
    assert.soon(() => changeStreamCursor.hasNext());
    changeStreamDoc = changeStreamCursor.next();
    assert.eq(changeStreamDoc.fullDocumentBeforeChange, expectedOnDeleteImages.preImage);
    assert(!changeStreamDoc.hasOwnProperty("fullDocument"), changeStreamDoc);
    assert.eq(changeStreamDoc.operationType, "delete");
    assertChangeStreamInternalFieldsNotPresent(changeStreamDoc);
}

preAndPostImageTest({
    expectedOnReplaceImages: {
        postImage: replacedDoc,
    },
    expectedOnDeleteImages: {}
});
preAndPostImageTest({
    changeStreamOptions: {fullDocumentBeforeChange: 'whenAvailable'},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled: {
        preImage: null,
    },
    expectedOnUpdateImages: {
        preImage: updatedDoc,
    },
    expectedOnReplaceImages: {
        preImage: updatedDoc2,
        postImage: replacedDoc,
    },
    expectedOnDeleteImages: {
        preImage: replacedDoc,
    }
});
preAndPostImageTest({
    changeStreamOptions: {fullDocument: 'whenAvailable'},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled: {
        postImage: null,
    },
    expectedOnUpdateImages: {
        postImage: updatedDoc2,
    },
    expectedOnReplaceImages: {
        postImage: replacedDoc,
    }
});
preAndPostImageTest({
    changeStreamOptions: {fullDocumentBeforeChange: 'whenAvailable', fullDocument: 'whenAvailable'},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled: {
        preImage: null,
        postImage: null,
    },
    expectedOnUpdateImages: {
        preImage: updatedDoc,
        postImage: updatedDoc2,
    },
    expectedOnReplaceImages: {
        preImage: updatedDoc2,
        postImage: replacedDoc,
    },
    expectedOnDeleteImages: {
        preImage: replacedDoc,
    }
});
preAndPostImageTest({
    changeStreamOptions: {fullDocumentBeforeChange: 'required'},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled: {} /* will throw on hasNext() */,
    expectedOnUpdateImages: {
        preImage: updatedDoc,
    },
    expectedOnReplaceImages: {
        preImage: updatedDoc2,
        postImage: replacedDoc,
    },
    expectedOnDeleteImages: {
        preImage: replacedDoc,
    }
});
preAndPostImageTest({
    changeStreamOptions: {fullDocument: 'required'},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled: {} /* will throw on hasNext() */,
    expectedOnUpdateImages: {
        postImage: updatedDoc2,
    },
    expectedOnReplaceImages: {
        postImage: replacedDoc,
    }
});
preAndPostImageTest({
    changeStreamOptions: {fullDocumentBeforeChange: 'required', fullDocument: 'required'},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled: {} /* will throw on hasNext() */,
    expectedOnUpdateImages: {
        preImage: updatedDoc,
        postImage: updatedDoc2,
    },
    expectedOnReplaceImages: {
        preImage: updatedDoc2,
        postImage: replacedDoc,
    },
    expectedOnDeleteImages: {
        preImage: replacedDoc,
    }
});
preAndPostImageTest({
    changeStreamOptions: {fullDocumentBeforeChange: 'whenAvailable', fullDocument: 'required'},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled: {} /* will throw on hasNext() */,
    expectedOnUpdateImages: {
        preImage: updatedDoc,
        postImage: updatedDoc2,
    },
    expectedOnReplaceImages: {
        preImage: updatedDoc2,
        postImage: replacedDoc,
    },
    expectedOnDeleteImages: {
        preImage: replacedDoc,
    }
});
preAndPostImageTest({
    changeStreamOptions: {fullDocumentBeforeChange: 'required', fullDocument: 'whenAvailable'},
    expectedOnUpdateImagesWithChangeStreamPreImagesDisabled: {} /* will throw on hasNext() */,
    expectedOnUpdateImages: {
        preImage: updatedDoc,
        postImage: updatedDoc2,
    },
    expectedOnReplaceImages: {
        preImage: updatedDoc2,
        postImage: replacedDoc,
    },
    expectedOnDeleteImages: {
        preImage: replacedDoc,
    }
});
}());
