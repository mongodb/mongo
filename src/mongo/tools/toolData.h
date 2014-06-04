#include "mongo/tools/tool.h"

typedef std::auto_ptr< Tool > (*InstanceFunction)();
typedef mongo::Status (*StoreOptions)(const mongo::moe::Environment& params, const std::vector<std::string>& args);
typedef bool (*HandleOptions)(const mongo::moe::Environment& params);
typedef mongo::Status (*AddOptions)(mongo::moe::OptionSection* options);

struct ToolData {
    InstanceFunction createFunction;
    StoreOptions store;
    HandleOptions handle;
    AddOptions add;
};