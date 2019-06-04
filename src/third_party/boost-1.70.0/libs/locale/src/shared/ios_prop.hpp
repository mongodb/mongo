//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_SRC_LOCALE_IOS_PROP_HPP
#define BOOST_SRC_LOCALE_IOS_PROP_HPP
#include <ios>

namespace boost {
    namespace locale {
        namespace impl {
       
            template<typename Property> 
            class ios_prop {
            public:
                static void set(Property const &prop,std::ios_base &ios)
                {
                    int id=get_id();
                    if(ios.pword(id)==0) {
                        ios.pword(id) = new Property(prop);
                        ios.register_callback(callback,id);
                    }
                    else if(ios.pword(id)==invalid) {
                        ios.pword(id) = new Property(prop);
                    }
                    else {
                        *static_cast<Property *>(ios.pword(id))=prop;
                    }
                }
                
                static Property &get(std::ios_base &ios)
                {
                    int id=get_id();
                    if(!has(ios))
                        set(Property(),ios);
                    return *static_cast<Property *>(ios.pword(id));
                }
                
                static bool has(std::ios_base &ios)
                {
                    int id=get_id();
                    if(ios.pword(id)==0 || ios.pword(id)==invalid)
                        return false;
                    return true;
                }

                static void unset(std::ios_base &ios)
                {
                    if(has(ios)) {
                        int id=get_id();
                        Property *p=static_cast<Property *>(ios.pword(id));
                        delete p;
                        ios.pword(id)=invalid;
                    }
                }
                static void global_init()
                {
                    get_id();
                }
            private:
                static void * const invalid;
                
                static void callback(std::ios_base::event ev,std::ios_base &ios,int id)
                {
                    switch(ev) {
                    case std::ios_base::erase_event:
                        if(!has(ios))
                            break;
                        delete reinterpret_cast<Property *>(ios.pword(id));
                        break;
                    case std::ios_base::copyfmt_event:
                        if(ios.pword(id)==invalid || ios.pword(id)==0)
                            break;
                        ios.pword(id)=new Property(*reinterpret_cast<Property *>(ios.pword(id)));
                        break;
                    case std::ios_base::imbue_event:
                        if(ios.pword(id)==invalid || ios.pword(id)==0)
                            break;
                        reinterpret_cast<Property *>(ios.pword(id))->on_imbue(); 
                        break;
                        
                    default: ;
                    }
                }
                static int get_id()
                {
                    static int id = std::ios_base::xalloc();
                    return id;
                }
            };

            template<typename Property>
            void * const ios_prop<Property>::invalid = (void *)(-1);



        }
    }
}



#endif

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4 

