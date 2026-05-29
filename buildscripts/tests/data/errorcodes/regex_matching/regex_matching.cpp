uassert(1, "words");
uasserted(2, "words");
massert(3, "words");
masserted(4, "words");
uassertNoTrace(5, "words");
uassertedNoTrace(6, "words");
massertNoTrace(7, "words");
massertedNoTrace(8, "words");
DBException(9, "words");
AssertionException(10, "words");
fassert(11, "words");
fassertFailed(12, "words");
fassertFailedWithStatus(13, "words");
fassertFailedWithStatusNoTrace(14, "words");
fassertFailedWithStatusNoTraceStatusOK(15, "words");
LOGV2(16, "words");
LOGV2_WARNING(17, "words");
logAndBackoff(18, "words");
ErrorCodes::Error(19);
ErrorCodes::Error{20};
LOGV2_ERROR(21,
            "words"
            "more words");
LOGV2_ERROR(22,
            "words",
            "comma, more words words words words words words words words words words words words ");
iassert(23, "words");
iasserted(24, "words");
iassertNoTrace(25, "words");
iassertedNoTrace(26, "words");
MONGO_UNREACHABLE_TASSERT(27);
MONGO_UNIMPLEMENTED_TASSERT(28);
