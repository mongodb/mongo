// This file replaces all the other _init files by letting the main Tool class decide which functions to call for argument parsing

#include "mongo/tools/tool.h"

#include "mongo/tools/tool_options.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo 
{
    Status _mongoInitializerFunction_MongoOptions_Register(InitializerContext*);
    namespace 
    { 
    	GlobalInitializerRegisterer _mongoInitializerRegisterer_MongoOptions_Register( "MongoOptions_Register", _mongoInitializerFunction_MongoOptions_Register, _makeStringVector(0, "BeginGeneralStartupOptionRegistration", NULL), _makeStringVector(0, "EndGeneralStartupOptionRegistration", NULL)); 
    }
    Status _mongoInitializerFunction_MongoOptions_Register(InitializerContext* context)
    {
        return Tool::addMongoOptions(&moe::startupOptions);
    }

    Status _mongoInitializerFunction_MongoOptions_Validate(InitializerContext*);
    namespace
    {
    	GlobalInitializerRegisterer _mongoInitializerRegisterer_MongoOptions_Validate( "MongoOptions_Validate", _mongoInitializerFunction_MongoOptions_Validate, _makeStringVector(0, "BeginStartupOptionValidation", NULL), _makeStringVector(0, "EndStartupOptionValidation", NULL));
    }
	Status _mongoInitializerFunction_MongoOptions_Validate(InitializerContext* context)
	{
	    if (!Tool::handlePreValidationMongoOptions(moe::startupOptionsParsed))
	    {
	        ::_exit(0);
	    }
	    Status ret = moe::startupOptionsParsed.validate();
	    if (!ret.isOK())
	    {
	        return ret;
	    }
	    return Status::OK();
    }

    Status _mongoInitializerFunction_MongoOptions_Store(InitializerContext*);
    namespace
    {
    	GlobalInitializerRegisterer _mongoInitializerRegisterer_MongoOptions_Store( "MongoOptions_Store", _mongoInitializerFunction_MongoOptions_Store, _makeStringVector(0, "BeginStartupOptionStorage", NULL), _makeStringVector(0, "EndStartupOptionStorage", NULL));
    }
	Status _mongoInitializerFunction_MongoOptions_Store(InitializerContext* context) 
	{
	    Status ret = Tool::storeMongoOptions(moe::startupOptionsParsed, context->args());
	    if (!ret.isOK()) {
	        std::cerr << ret.toString() << std::endl;
	        std::cerr << "try '" << context->args()[0] << " --help' for more information"
	                  << std::endl;
	        ::_exit(EXIT_BADOPTIONS);
	    }
	    return Status::OK();
    }
}