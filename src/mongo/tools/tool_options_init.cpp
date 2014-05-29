#include "mongo/tools/mongotop_options.h"

#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo 
{
    Status _mongoInitializerFunction_MongoTopOptions_Register(InitializerContext*);
    namespace 
    { 
    	GlobalInitializerRegisterer _mongoInitializerRegisterer_MongoTopOptions_Register( "MongoTopOptions_Register", _mongoInitializerFunction_MongoTopOptions_Register, _makeStringVector(0, "BeginGeneralStartupOptionRegistration", __null), _makeStringVector(0, "EndGeneralStartupOptionRegistration", __null)); 
    }
    Status _mongoInitializerFunction_MongoTopOptions_Register(InitializerContext* context)
    {
        return Tool::addMongoOptions(&moe::startupOptions);
    }

    Status _mongoInitializerFunction_MongoTopOptions_Validate(InitializerContext*);
    namespace
    {
    	GlobalInitializerRegisterer _mongoInitializerRegisterer_MongoTopOptions_Validate( "MongoTopOptions_Validate", _mongoInitializerFunction_MongoTopOptions_Validate, _makeStringVector(0, "BeginStartupOptionValidation", __null), _makeStringVector(0, "EndStartupOptionValidation", __null));
    }
	Status _mongoInitializerFunction_MongoTopOptions_Validate(InitializerContext* context)
	{
	    if (!handlePreValidationMongoTopOptions(moe::startupOptionsParsed))
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

    Status _mongoInitializerFunction_MongoTopOptions_Store(InitializerContext*);
    namespace
    {
    	GlobalInitializerRegisterer _mongoInitializerRegisterer_MongoTopOptions_Store( "MongoTopOptions_Store", _mongoInitializerFunction_MongoTopOptions_Store, _makeStringVector(0, "BeginStartupOptionStorage", __null), _makeStringVector(0, "EndStartupOptionStorage", __null));
    }
	Status _mongoInitializerFunction_MongoTopOptions_Store(InitializerContext* context) 
	{
	    Status ret = storeMongoTopOptions(moe::startupOptionsParsed, context->args());
	    if (!ret.isOK()) {
	        std::cerr << ret.toString() << std::endl;
	        std::cerr << "try '" << context->args()[0] << " --help' for more information"
	                  << std::endl;
	        ::_exit(EXIT_BADOPTIONS);
	    }
	    return Status::OK();
    }
}