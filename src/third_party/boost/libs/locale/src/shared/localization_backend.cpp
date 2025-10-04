//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/localization_backend.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <functional>
#include <memory>
#include <vector>

#ifdef BOOST_LOCALE_WITH_ICU
#    include "../icu/icu_backend.hpp"
#endif

#ifndef BOOST_LOCALE_NO_POSIX_BACKEND
#    include "../posix/posix_backend.hpp"
#endif

#ifndef BOOST_LOCALE_NO_STD_BACKEND
#    include "../std/std_backend.hpp"
#endif

#ifndef BOOST_LOCALE_NO_WINAPI_BACKEND
#    include "../win32/win_backend.hpp"
#endif

namespace boost { namespace locale {
    static std::unique_ptr<localization_backend> clone(const localization_backend& backend)
    {
        return std::unique_ptr<localization_backend>(backend.clone());
    }

    localization_backend::~localization_backend() = default;

    class localization_backend_manager::impl {
    public:
        impl(const impl& other) : default_backends_(other.default_backends_)
        {
            for(const auto& i : other.all_backends_)
                all_backends_.push_back({i.first, clone(*i.second)});
        }
        impl() : default_backends_(32, -1) {}

        impl& operator=(const impl&) = delete;

        localization_backend* create() const
        {
            std::vector<std::reference_wrapper<const localization_backend>> backends;
            for(const auto& i : all_backends_)
                backends.push_back(std::cref(*i.second));
            return new actual_backend(backends, default_backends_);
        }

        int find_backend(const std::string& name) const
        {
            int id = 0;
            for(const auto& i : all_backends_) {
                if(i.first == name)
                    return id;
                ++id;
            }
            return -1;
        }

        void add_backend(const std::string& name, std::unique_ptr<localization_backend> ptr)
        {
            if(all_backends_.empty())
                std::fill(default_backends_.begin(), default_backends_.end(), 0);
            if(BOOST_LIKELY(find_backend(name) < 0))
                all_backends_.push_back(std::make_pair(name, std::move(ptr)));
        }

        void select(const std::string& backend_name, category_t category = all_categories)
        {
            const int id = find_backend(backend_name);
            if(id >= 0) {
                category_t flag = category_first;
                for(int& defBackend : default_backends_) {
                    if(category & flag)
                        defBackend = id;
                    ++flag;
                }
            }
        }

        void remove_all_backends()
        {
            all_backends_.clear();
            std::fill(default_backends_.begin(), default_backends_.end(), -1);
        }
        std::vector<std::string> get_all_backends() const
        {
            std::vector<std::string> res;
            for(const auto& i : all_backends_)
                res.push_back(i.first);
            return res;
        }

    private:
        class actual_backend : public localization_backend {
        public:
            actual_backend(const std::vector<std::reference_wrapper<const localization_backend>>& backends,
                           const std::vector<int>& index) :
                index_(index)
            {
                for(const localization_backend& b : backends)
                    backends_.push_back(boost::locale::clone(b));
            }
            actual_backend* clone() const override
            {
                std::vector<std::reference_wrapper<const localization_backend>> backends;
                for(const auto& b : backends_)
                    backends.push_back(std::cref(*b));
                return new actual_backend(backends, index_);
            }
            void set_option(const std::string& name, const std::string& value) override
            {
                for(const auto& b : backends_)
                    b->set_option(name, value);
            }
            void clear_options() override
            {
                for(const auto& b : backends_)
                    b->clear_options();
            }
            std::locale install(const std::locale& l, category_t category, char_facet_t type) override
            {
                unsigned id = 0;
                for(category_t v = category_first; v != category; ++v, ++id) {
                    if(v == category_last)
                        return l;
                }
                if(id >= index_.size() || index_[id] == -1)
                    return l;
                return backends_[index_[id]]->install(l, category, type);
            }

        private:
            std::vector<std::unique_ptr<localization_backend>> backends_;
            std::vector<int> index_;
        };

        std::vector<std::pair<std::string, std::unique_ptr<localization_backend>>> all_backends_;
        std::vector<int> default_backends_;
    };

    localization_backend_manager::localization_backend_manager() : pimpl_(new impl()) {}

    localization_backend_manager::~localization_backend_manager() = default;

    localization_backend_manager::localization_backend_manager(const localization_backend_manager& other) :
        pimpl_(new impl(*other.pimpl_))
    {}

    localization_backend_manager& localization_backend_manager::operator=(const localization_backend_manager& other)
    {
        pimpl_.reset(new impl(*other.pimpl_));
        return *this;
    }

    localization_backend_manager::localization_backend_manager(localization_backend_manager&&) noexcept = default;
    localization_backend_manager&
    localization_backend_manager::operator=(localization_backend_manager&&) noexcept = default;

    std::unique_ptr<localization_backend> localization_backend_manager::create() const
    {
        return std::unique_ptr<localization_backend>(pimpl_->create());
    }
    void localization_backend_manager::add_backend(const std::string& name,
                                                   std::unique_ptr<localization_backend> backend)
    {
        pimpl_->add_backend(name, std::move(backend));
    }

    void localization_backend_manager::remove_all_backends()
    {
        pimpl_->remove_all_backends();
    }
    std::vector<std::string> localization_backend_manager::get_all_backends() const
    {
        return pimpl_->get_all_backends();
    }
    void localization_backend_manager::select(const std::string& backend_name, category_t category)
    {
        pimpl_->select(backend_name, category);
    }

    namespace {
        localization_backend_manager make_default_backend_mgr()
        {
            localization_backend_manager mgr;
#ifdef BOOST_LOCALE_WITH_ICU
            mgr.add_backend("icu", impl_icu::create_localization_backend());
#endif

#ifndef BOOST_LOCALE_NO_POSIX_BACKEND
            mgr.add_backend("posix", impl_posix::create_localization_backend());
#endif

#ifndef BOOST_LOCALE_NO_WINAPI_BACKEND
            mgr.add_backend("winapi", impl_win::create_localization_backend());
#endif

#ifndef BOOST_LOCALE_NO_STD_BACKEND
            mgr.add_backend("std", impl_std::create_localization_backend());
#endif

            return mgr;
        }

        boost::mutex& localization_backend_manager_mutex()
        {
            static boost::mutex the_mutex;
            return the_mutex;
        }
        localization_backend_manager& localization_backend_manager_global()
        {
            static localization_backend_manager the_manager = make_default_backend_mgr();
            return the_manager;
        }
    } // namespace

    localization_backend_manager localization_backend_manager::global()
    {
        boost::unique_lock<boost::mutex> lock(localization_backend_manager_mutex());
        return localization_backend_manager_global();
    }
    localization_backend_manager localization_backend_manager::global(const localization_backend_manager& in)
    {
        boost::unique_lock<boost::mutex> lock(localization_backend_manager_mutex());
        return exchange(localization_backend_manager_global(), in);
    }

}} // namespace boost::locale
