//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/locale/localization_backend.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/locale/hold_ptr.hpp>
#include <vector>

#ifdef BOOST_LOCALE_WITH_ICU
#include "../icu/icu_backend.hpp"
#endif

#ifndef BOOST_LOCALE_NO_POSIX_BACKEND
#include "../posix/posix_backend.hpp"
#endif

#ifndef BOOST_LOCALE_NO_STD_BACKEND
#include "../std/std_backend.hpp"
#endif

#ifndef BOOST_LOCALE_NO_WINAPI_BACKEND
#include "../win32/win_backend.hpp"
#endif

namespace boost {
    namespace locale {
        class localization_backend_manager::impl {
            void operator = (impl const &);
        public:
            impl(impl const &other) :
                default_backends_(other.default_backends_)
            {
                for(all_backends_type::const_iterator p=other.all_backends_.begin();p!=other.all_backends_.end();++p) {
                    all_backends_type::value_type v;
                    v.first=p->first;
                    v.second.reset(p->second->clone());
                    all_backends_.push_back(v);
                }
            }
            impl() : 
                default_backends_(32,-1)
            {
            }

            localization_backend *create() const
            {
                std::vector<boost::shared_ptr<localization_backend> > backends;
                for(unsigned i=0;i<all_backends_.size();i++)
                    backends.push_back(all_backends_[i].second);
                return new actual_backend(backends,default_backends_);
            }
            void adopt_backend(std::string const &name,localization_backend *backend_ptr)
            {
                boost::shared_ptr<localization_backend> sptr(backend_ptr);
                if(all_backends_.empty()) {
                    all_backends_.push_back(std::make_pair(name,sptr));
                    for(unsigned i=0;i<default_backends_.size();i++)
                        default_backends_[i]=0;
                }
                else { 
                    for(unsigned i=0;i<all_backends_.size();i++)
                        if(all_backends_[i].first == name)
                            return;
                    all_backends_.push_back(std::make_pair(name,sptr));
                }
            }

            void select(std::string const &backend_name,locale_category_type category = all_categories)
            {
                unsigned id;
                for(id=0;id<all_backends_.size();id++) {
                    if(all_backends_[id].first == backend_name)
                        break;
                }
                if(id==all_backends_.size())
                    return;
                for(unsigned flag = 1,i=0;i<default_backends_.size();flag <<=1,i++) {
                    if(category & flag) {
                        default_backends_[i]=id;
                    }
                }
            }

            void remove_all_backends()
            {
                all_backends_.clear();
                for(unsigned i=0;i<default_backends_.size();i++) {
                    default_backends_[i]=-1;
                }
            }
            std::vector<std::string> get_all_backends() const
            {
                std::vector<std::string> res;
                all_backends_type::const_iterator  p;
                for(p=all_backends_.begin();p!=all_backends_.end();++p) {
                    res.push_back(p->first);
                }
                return res;
            }

        private:
            class actual_backend : public localization_backend {
            public:
                actual_backend(std::vector<boost::shared_ptr<localization_backend> > const &backends,std::vector<int> const &index):
                    index_(index)
                {
                    backends_.resize(backends.size());
                    for(unsigned i=0;i<backends.size();i++) {
                        backends_[i].reset(backends[i]->clone());
                    }
                }
                virtual actual_backend *clone() const 
                {
                    return new actual_backend(backends_,index_);
                }
                virtual void set_option(std::string const &name,std::string const &value)
                {
                    for(unsigned i=0;i<backends_.size();i++)
                        backends_[i]->set_option(name,value);
                }
                virtual void clear_options()
                {
                    for(unsigned i=0;i<backends_.size();i++)
                        backends_[i]->clear_options();
                }
                virtual std::locale install(std::locale const &l,locale_category_type category,character_facet_type type = nochar_facet)
                {
                    int id;
                    unsigned v;
                    for(v=1,id=0;v!=0;v<<=1,id++) {
                        if(category == v)
                            break;
                    }
                    if(v==0)
                        return l;
                    if(unsigned(id) >= index_.size())
                        return l;
                    if(index_[id]==-1) 
                        return l;
                    return backends_[index_[id]]->install(l,category,type);
                }
            private:
                std::vector<boost::shared_ptr<localization_backend> > backends_;
                std::vector<int> index_;
            };

            typedef std::vector<std::pair<std::string,boost::shared_ptr<localization_backend> > > all_backends_type;
            all_backends_type all_backends_;
            std::vector<int> default_backends_;
        };



        localization_backend_manager::localization_backend_manager() : 
            pimpl_(new impl())
        {
        }

        localization_backend_manager::~localization_backend_manager()
        {
        }

        localization_backend_manager::localization_backend_manager(localization_backend_manager const &other) :
            pimpl_(new impl(*other.pimpl_))
        {
        }

        localization_backend_manager const &localization_backend_manager::operator = (localization_backend_manager const &other)
        {
            if(this!=&other) {
                pimpl_.reset(new impl(*other.pimpl_));
            }
            return *this;
        }


        #if !defined(BOOST_LOCALE_HIDE_AUTO_PTR) && !defined(BOOST_NO_AUTO_PTR)
        std::auto_ptr<localization_backend> localization_backend_manager::get() const
        {
            std::auto_ptr<localization_backend> r(pimpl_->create());
            return r;
        }
        void localization_backend_manager::add_backend(std::string const &name,std::auto_ptr<localization_backend> backend)
        {
            pimpl_->adopt_backend(name,backend.release());
        }
        #endif
        #ifndef BOOST_NO_CXX11_SMART_PTR
        std::unique_ptr<localization_backend> localization_backend_manager::get_unique_ptr() const
        {
            std::unique_ptr<localization_backend> r(pimpl_->create());
            return r;
        }
        void localization_backend_manager::add_backend(std::string const &name,std::unique_ptr<localization_backend> backend)
        {
            pimpl_->adopt_backend(name,backend.release());
        }
        #endif
        localization_backend *localization_backend_manager::create() const
        {
            return pimpl_->create();
        }
        void localization_backend_manager::adopt_backend(std::string const &name,localization_backend *backend)
        {
            pimpl_->adopt_backend(name,backend);
        }


        void localization_backend_manager::remove_all_backends()
        {
            pimpl_->remove_all_backends();
        }
        std::vector<std::string> localization_backend_manager::get_all_backends() const
        {
            return pimpl_->get_all_backends();
        }
        void localization_backend_manager::select(std::string const &backend_name,locale_category_type category)
        {
            pimpl_->select(backend_name,category);
        }

        namespace {
            // prevent initialization order fiasco
            boost::mutex &localization_backend_manager_mutex()
            {
                static boost::mutex the_mutex;
                return the_mutex;
            }
            // prevent initialization order fiasco
            localization_backend_manager &localization_backend_manager_global()
            {
                static localization_backend_manager the_manager;
                return the_manager;
            }

            struct init {
                init() { 
                    localization_backend_manager mgr;
                    #ifdef BOOST_LOCALE_WITH_ICU
                    mgr.adopt_backend("icu",impl_icu::create_localization_backend());
                    #endif

                    #ifndef BOOST_LOCALE_NO_POSIX_BACKEND
                    mgr.adopt_backend("posix",impl_posix::create_localization_backend());
                    #endif

                    #ifndef BOOST_LOCALE_NO_WINAPI_BACKEND
                    mgr.adopt_backend("winapi",impl_win::create_localization_backend());
                    #endif
                    
                    #ifndef BOOST_LOCALE_NO_STD_BACKEND
                    mgr.adopt_backend("std",impl_std::create_localization_backend());
                    #endif

                    localization_backend_manager::global(mgr);
                }
            } do_init;
        }

        localization_backend_manager localization_backend_manager::global()
        {
            boost::unique_lock<boost::mutex> lock(localization_backend_manager_mutex());
            localization_backend_manager mgr = localization_backend_manager_global();
            return mgr;
        }
        localization_backend_manager localization_backend_manager::global(localization_backend_manager const &in)
        {
            boost::unique_lock<boost::mutex> lock(localization_backend_manager_mutex());
            localization_backend_manager mgr = localization_backend_manager_global();
            localization_backend_manager_global() = in;
            return mgr;
        }



    } // locale
} // boost
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4 
