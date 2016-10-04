/**
 * Sample file to demonstrate various clang-format messages
 */
#include <stdio.h>
#include <vector>
#include <string>

// NamespaceIndentation: None
namespace foo {
// Contents of namespace are not indented.
int foo;

namespace bar {
int bar;

namespace {
int anony;
}  // namespace anony

}  // namespace bar

/**
 * A Class comment
 */
class Example {
    // AccessModiferOffset: -4
public:
    // BreakConstructorInitializersBeforeComma: false
    // ConstructorInitializerAllOnOneLineOrOnePerLine: true
    //
    Example() : _aVariable(4), _bVariable(42) {}
    Example(int a)
        : _aVariable(a),
          _bVariable(42),
          _AReallyReallyLongVariableName(4),
          _AnotherReallyReallyLongVariableNameToTriggerWrapping(42) {
        printf("Hello ");
    }
    ~Example() {}

    /**
     * A Function comment
     * AllowShortFunctionsOnASingleLine: Empty
     */
    int getOneLineFunction() {
        return 0;
    }

    /** A Incorrect Function comment
     * AllowShortFunctionsOnASingleLine: Empty
     */
    void doNothing() {}

    /**
     * A Useful Function comment
     */
    int manyVariableFunction(unsigned long long arg1, char arg2, unsigned long long arg3);

private:
    int _aVariable;
    long _bVariable;
    short _cVarianble = 49;
    long _AReallyReallyLongVariableName;
    long _AnotherReallyReallyLongVariableNameToTriggerWrapping;
};

int foo3() {
    return 42;
}

// AlwaysBreakTemplateDeclarations: true
template <typename T>
T myAdd(T x, T y) {
    return x + y;
}

// AlwaysBreakAfterDefinitionReturnType: false
// BinPackParameters: false
int Example::manyVariableFunction(unsigned long long argWithLongName,
                                  char arg2,
                                  unsigned long long argWithAnotherLongName) {
    // 3.7 - AlignConsecutiveAssignments - false
    //
    int aaaa = 12;
    int b = 23;
    int ccc = 23;
    // PointerAlignment: Left
    const char* some_pointer = "Hello";

    // SpacesInAngles: false
    std::vector<std::pair<std::string, int>> list;

    // SpaceAfterCStyleCast: false
    // SpacesInCStyleCastParentheses: false
    char* some_nonconst_pointer = (char*)some_pointer;

    // Multi-line if
    // SpaceBeforeParens: False
    if (argWithLongName == 0) {  // Comment: SpacesBeforeTrailingComments
        // Do something
    } else if (b % 7 = 3) {
    }  // some weird trailing else comment that clang-format does not touch
    else {
        // Notice the indent around else
    }

    // AllowShortIfStatementsOnASingleLine: false
    // Put statements on separate lines for short ifs
    if (arg2 == 'a')
        arg2 = 'b';

    int bbbbbbbbbbbbbbbbbbbbbbbbbbbb, cccccccccccccccccccccccccccccc;
    int aaaaaaaaaaaaaaaaaaaaaaaaaaaa =
        bbbbbbbbbbbbbbbbbbbbbbbbbbbb + cccccccccccccccccccccccccccccc;

    // AlignOperands
    int dddddddddddddddddddddddddddd = aaaaaaaaaaaaaaaaaaaaaaaaaaaa * 7 + 4 % 235124 > 275645
        ? bbbbbbbbbbbbbbbbbbbbbbbbbbbb + 897234
        : cccccccccccccccccccccccccccccc % 1293402;

    // AllowShortBlocksOnASingleLine: false
    if (b) {
        return 3;
    }

    // AllowShortLoopsOnASingleLine: false
    while (b < 5)
        b++;

    // BreakBeforeBinaryOperators: None
    if (b > 5 || b % 42 || cccccccccccccccccccccccccccccc % 873 || aaaa * 12312 % 23485 != 9873 ||
        some_pointer != 0) {
        printf("Huh!\n");
    }

    // AlignAfterOpenBracket: false
    // BinPackParameters: false
    printf("A short function call %s %s %d - %ld\n", "", "tgz", 4, ULONG_MAX);
    printf("A long function call %s %s %d - %ld\n",
           "http://www.mongodbo.org/downloads",
           "mongodb-latest.tgz",
           4,
           ULONG_MAX);
    printf("Thing1 %s\n", "Thing2");

    // No spaces between parens and args
    printf("%c\n", arg2);

    // A switch statement: TODO: Andy, what is the indent we want? Google style?
    switch (arg2) {
        // AllowShortCaseLabelsOnASingleLine: false
        // IndentCaseLabels: true
        case 'a':
            return 2;
        case 'y':
        case 'z':
            // Do something here
            break;
        default:
            // The default`
            break;
    }

    do {
        // Do a loop here
    } while (0);

    return 1;
}

}  // namespace foo
