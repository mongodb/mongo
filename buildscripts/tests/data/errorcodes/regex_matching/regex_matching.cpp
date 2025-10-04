uassert(1, "words");
uasserted(2, "words");
massert(3, "words");
masserted(4, "words");
msgassert(5, "words");
msgasserted(6, "words");
uassertNoTrace(7, "words");
uassertedNoTrace(8, "words");
massertNoTrace(9, "words");
massertedNoTrace(10, "words");
msgassertNoTrace(11, "words");
msgassertedNoTrace(12, "words");
DBException(13, "words");
AssertionException(14, "words");
fassert(15, "words");
fassertFailed(16, "words");
fassertFailedWithStatus(17, "words");
fassertFailedWithStatusNoTrace(18, "words");
fassertFailedWithStatusNoTraceStatusOK(19, "words");
LOGV2(20, "words");
LOGV2_WARNING(21, "words");
logAndBackoff(22, "words");
ErrorCodes::Error(23);
ErrorCodes::Error{24};
LOGV2_ERROR(25,
            "words"
            "more words");
LOGV2_ERROR(26,
            "words",
            "comma, more words words words words words words words words words words words words ");
iassert(27, "words");
iasserted(28, "words");
iassertNoTrace(29, "words");
iassertedNoTrace(30, "words");
MONGO_UNREACHABLE_TASSERT(31);
MONGO_UNIMPLEMENTED_TASSERT(32);
