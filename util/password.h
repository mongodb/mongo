/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */


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
