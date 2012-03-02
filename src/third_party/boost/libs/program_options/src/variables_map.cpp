// Copyright Vladimir Prus 2002-2004.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)


#define BOOST_PROGRAM_OPTIONS_SOURCE
#include <boost/program_options/config.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>

#include <cassert>

namespace boost { namespace program_options {

    using namespace std;

    // First, performs semantic actions for 'oa'. 
    // Then, stores in 'm' all options that are defined in 'desc'. 
    BOOST_PROGRAM_OPTIONS_DECL 
    void store(const parsed_options& options, variables_map& xm,
               bool utf8)
    {       
        // TODO: what if we have different definition
        // for the same option name during different calls
        // 'store'.
        assert(options.description);
        const options_description& desc = *options.description;

        // We need to access map's operator[], not the overriden version
        // variables_map. Ehmm.. messy.
        std::map<std::string, variable_value>& m = xm;

        std::set<std::string> new_final;

        // Declared once, to please Intel in VC++ mode;
        unsigned i;

        // First, convert/store all given options
        for (i = 0; i < options.options.size(); ++i) {

            const string& name = options.options[i].string_key;
            // Skip positional options without name
            if (name.empty())
                continue;

            // Ignore unregistered option. The 'unregistered'
            // field can be true only if user has explicitly asked
            // to allow unregistered options. We can't store them
            // to variables map (lacking any information about paring), 
            // so just ignore them.
            if (options.options[i].unregistered)
                continue;

            // If option has final value, skip this assignment
            if (xm.m_final.count(name))
                continue;

            const option_description& d = desc.find(name, false, 
                                                      false, false);

            variable_value& v = m[name];            
            if (v.defaulted()) {
                // Explicit assignment here erases defaulted value
                v = variable_value();
            }
            
            try {
                d.semantic()->parse(v.value(), options.options[i].value, utf8);
            }
#ifndef BOOST_NO_EXCEPTIONS
            catch(validation_error& e)
            {
                e.set_option_name(name);
                throw;
            }
            catch(multiple_occurrences& e)
            {
                e.set_option_name(name);
                throw;
            }
            catch(multiple_values& e) 
            {
                e.set_option_name(name);
                throw;
            }
#endif
            v.m_value_semantic = d.semantic();
            
            // The option is not composing, and the value is explicitly
            // provided. Ignore values of this option for subsequent
            // calls to 'store'. We store this to a temporary set,
            // so that several assignment inside *this* 'store' call
            // are allowed.
            if (!d.semantic()->is_composing())
                new_final.insert(name);
        }
        xm.m_final.insert(new_final.begin(), new_final.end());

        
        
        // Second, apply default values and store required options.
        const vector<shared_ptr<option_description> >& all = desc.options();
        for(i = 0; i < all.size(); ++i)
        {
            const option_description& d = *all[i];
            string key = d.key("");
            // FIXME: this logic relies on knowledge of option_description
            // internals.
            // The 'key' is empty if options description contains '*'. 
            // In that 
            // case, default value makes no sense at all.
            if (key.empty())
            {
                continue;
            }
            if (m.count(key) == 0) {
            
                boost::any def;
                if (d.semantic()->apply_default(def)) {
                    m[key] = variable_value(def, true);
                    m[key].m_value_semantic = d.semantic();
                }
            }  

            // add empty value if this is an required option
            if (d.semantic()->is_required()) {
               xm.m_required.insert(key);
            }
        }
    }

    BOOST_PROGRAM_OPTIONS_DECL 
    void store(const wparsed_options& options, variables_map& m)
    {
        store(options.utf8_encoded_options, m, true);
    }

    BOOST_PROGRAM_OPTIONS_DECL 
    void notify(variables_map& vm)
    {        
        vm.notify();               
    }

    abstract_variables_map::abstract_variables_map()
    : m_next(0)
    {}

    abstract_variables_map::
    abstract_variables_map(const abstract_variables_map* next)
    : m_next(next)
    {}

    const variable_value& 
    abstract_variables_map::operator[](const std::string& name) const
    {
        const variable_value& v = get(name);
        if (v.empty() && m_next)
            return (*m_next)[name];
        else if (v.defaulted() && m_next) {
            const variable_value& v2 = (*m_next)[name];
            if (!v2.empty() && !v2.defaulted())
                return v2;
            else return v;
        } else {
            return v;
        }
    }

    void 
    abstract_variables_map::next(abstract_variables_map* next)
    {
        m_next = next;
    }

    variables_map::variables_map()
    {}

    variables_map::variables_map(const abstract_variables_map* next)
    : abstract_variables_map(next)
    {}

    const variable_value&
    variables_map::get(const std::string& name) const
    {
        static variable_value empty;
        const_iterator i = this->find(name);
        if (i == this->end())
            return empty;
        else
            return i->second;
    }
    
    void
    variables_map::notify()
    {
        // This checks if all required options occur
        for (set<string>::const_iterator r = m_required.begin();
             r != m_required.end();
             ++r)
        {
            const string& opt = *r;
            map<string, variable_value>::const_iterator iter = find(opt);
            if (iter == end() || iter->second.empty()) 
            {
                boost::throw_exception(required_option(opt));
            
            }
        }

        // Lastly, run notify actions.
        for (map<string, variable_value>::iterator k = begin(); 
             k != end(); 
             ++k) 
        {
            /* Users might wish to use variables_map to store their own values
               that are not parsed, and therefore will not have value_semantics
               defined. Do no crash on such values. In multi-module programs,
               one module might add custom values, and the 'notify' function
               will be called after that, so we check that value_sematics is 
               not NULL. See:
                   https://svn.boost.org/trac/boost/ticket/2782
            */
            if (k->second.m_value_semantic)
                k->second.m_value_semantic->notify(k->second.value());
        }               
    }
    
}}
