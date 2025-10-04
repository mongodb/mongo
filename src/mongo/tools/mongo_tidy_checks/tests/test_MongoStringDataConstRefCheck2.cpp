namespace mongo {
class StringData {};

int func(const StringData&& sd) {
    return -1;
}
}  // namespace mongo
