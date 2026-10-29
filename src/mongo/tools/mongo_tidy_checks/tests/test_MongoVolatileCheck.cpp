namespace mongo {

volatile int varVolatileTest;
class testClass {
    volatile int fieldVolatileTest;
};
void functionName(volatile int varVolatileTest) {}

}  // namespace mongo
