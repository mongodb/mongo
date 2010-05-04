
#pragma once

#include <boost/program_options.hpp>
#include <string>

namespace mongo {

    struct PasswordValue : public boost::program_options::typed_value<std::string> {

        PasswordValue( std::string* val )
            : boost::program_options::typed_value<std::string>( val ) { }

        unsigned min_tokens() const {
            return 0;
        }

        unsigned max_tokens() const {
            return 1;
        }

        bool is_required() const {
            return false;
        }

        void xparse( boost::any& value_store, 
                     const std::vector<std::string>& new_tokens ) const {
            if ( !value_store.empty() )
#if BOOST_VERSION >= 104200
                boost::throw_exception( boost::program_options::validation_error( boost::program_options::validation_error::multiple_values_not_allowed ) );
#else
                boost::throw_exception( boost::program_options::validation_error( "multiple values not allowed" ) );
#endif
            else if ( !new_tokens.empty() )
                boost::program_options::typed_value<std::string>::xparse
                    (value_store, new_tokens);
            else
                value_store = std::string();
        }

    };

    std::string askPassword();

}
