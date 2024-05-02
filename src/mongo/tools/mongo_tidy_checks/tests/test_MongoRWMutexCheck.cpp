namespace mongo {

class WriteRarelyRWMutex {};

WriteRarelyRWMutex mutex_vardecl;

struct MyStruct {
    WriteRarelyRWMutex mutex_fielddecl;
};

}  // namespace mongo
