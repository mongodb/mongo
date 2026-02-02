#pragma once

#include <immer/extra/io.hpp>
#include <immer/extra/persist/cereal/archives.hpp>
#include <immer/extra/persist/detail/names.hpp>
#include <immer/extra/persist/detail/traits.hpp>
#include <immer/extra/persist/errors.hpp>

#include <boost/hana.hpp>

#include <optional>

namespace immer::persist::detail {

namespace hana = boost::hana;

/**
 * Unimplemented class to generate a compile-time error and show what the type T
 * is.
 */
template <class T>
class error_no_pool_for_the_given_type_check_get_pools_types_function;

template <class T>
class error_missing_pool_for_type;

template <class T>
struct storage_holder
{
    T value;

    auto& operator()() { return value; }
    const auto& operator()() const { return value; }
};

template <class T>
auto make_storage_holder(T&& value)
{
    return storage_holder<std::decay_t<T>>{std::forward<T>(value)};
}

template <class T>
struct shared_storage_holder
{
    std::shared_ptr<T> ptr;

    auto& operator()() { return *ptr; }
    const auto& operator()() const { return *ptr; }
};

template <class T>
auto make_shared_storage_holder(std::shared_ptr<T> ptr)
{
    return shared_storage_holder<T>{std::move(ptr)};
}

/**
 * Pools and functions to serialize types that contain persistable data
 * structures.
 */
template <class Storage>
struct output_pools
{
    Storage storage_;

    // To align the interface with input_pools
    Storage& storage() { return storage_; }
    const Storage& storage() const { return storage_; }

    template <class Archive>
    void save(Archive& ar) const
    {
        using pool_name_fn = typename Archive::pool_name_fn;
        using keys_t       = decltype(hana::keys(storage()));
        auto used_names    = std::unordered_set<std::string>{};
        hana::for_each(keys_t{}, [&](auto key) {
            using Container = typename decltype(key)::type;
            using NameType  = decltype(pool_name_fn{}(Container{}));
            if constexpr (std::is_void_v<NameType>) {
                ar(storage()[key]);
            } else {
                const auto& name                = pool_name_fn{}(Container{});
                const auto [iterator, inserted] = used_names.insert(name);
                if (!inserted) {
                    throw duplicate_name_pool_detected{name};
                }
                ar(cereal::make_nvp(name, storage()[key]));
            }
        });
    }

    template <class T>
    auto& get_output_pool()
    {
        using Contains = decltype(hana::contains(storage(), hana::type_c<T>));
        if constexpr (!Contains::value) {
            auto err =
                error_no_pool_for_the_given_type_check_get_pools_types_function<
                    T>{};
        }
        return storage()[hana::type_c<T>];
    }

    template <class T>
    const auto& get_output_pool() const
    {
        using Contains = decltype(hana::contains(storage(), hana::type_c<T>));
        if constexpr (!Contains::value) {
            auto err =
                error_no_pool_for_the_given_type_check_get_pools_types_function<
                    T>{};
        }
        return storage()[hana::type_c<T>];
    }

    friend bool operator==(const output_pools& left, const output_pools& right)
    {
        return left.storage() == right.storage();
    }
};

template <class Derived, class Loader>
struct no_loader
{};

template <class Derived, class Loader>
struct with_loader
{
    std::optional<Loader> loader;

    auto& get_loader_from_per_type_pool()
    {
        if (!loader) {
            auto& self = static_cast<Derived&>(*this);
            loader.emplace(self.pool, self.transform);
        }
        return *loader;
    }
};

/**
 * A pool for one container type.
 * Normally, the pool does not contain a loader, which is located inside the
 * input_pools_cereal_archive_wrapper.
 *
 * But in case of transformations, there is no
 * input_pools_cereal_archive_wrapper involved and it becomes convenient to have
 * the corresponding loader stored here, too, via with_loader.
 */
template <class Container,
          class Pool       = typename container_traits<Container>::input_pool_t,
          class TransformF = boost::hana::id_t,
          class OldContainerType                    = boost::hana::id_t,
          template <class, class> class LoaderMixin = no_loader>
struct input_pool
    : LoaderMixin<input_pool<Container,
                             Pool,
                             TransformF,
                             OldContainerType,
                             LoaderMixin>,
                  typename container_traits<
                      Container>::template loader_t<Pool, TransformF>>
{
    using container_t     = Container;
    using old_container_t = OldContainerType;

    Pool pool = {};
    TransformF transform;

    input_pool() = default;

    explicit input_pool(Pool pool_)
        : pool{std::move(pool_)}
    {
    }

    explicit input_pool(Pool pool_, TransformF transform_)
        : pool{std::move(pool_)}
        , transform{std::move(transform_)}
    {
    }

    template <class Func>
    auto with_transform(Func&& func) const
    {
        using value_type = typename Container::value_type;
        using new_value_type =
            std::decay_t<decltype(func(std::declval<value_type>()))>;
        using NewContainer =
            std::decay_t<decltype(container_traits<Container>::transform(
                func))>;
        using TransF = std::function<new_value_type(const value_type&)>;
        // the transform function must be filled in later
        return input_pool<NewContainer, Pool, TransF, Container, with_loader>{
            pool, TransF{}};
    }

    friend bool operator==(const input_pool& left, const input_pool& right)
    {
        return left.pool == right.pool;
    }

    friend bool operator!=(const input_pool& left, const input_pool& right)
    {
        return left.pool != right.pool;
    }

    void merge_previous(const input_pool& original)
    {
        pool.merge_previous(original.pool);
    }

    static auto generate_loader()
    {
        return std::optional<typename container_traits<
            Container>::template loader_t<Pool, TransformF>>{};
    }
};

/**
 * Transforms a given function into another function that:
 *   - If the given function is a function of one argument, nothing changes.
 *   - Otherwise, passes the given argument as the second argument for the
 * function.
 *
 * In other words, takes a function of maybe two arguments and returns a
 * function of just one argument.
 */
constexpr auto inject_argument = [](auto arg, auto func) {
    return [arg = std::move(arg), func = std::move(func)](auto&& old) {
        const auto is_valid = hana::is_valid(func, old);
        if constexpr (std::decay_t<decltype(is_valid)>::value) {
            return func(old);
        } else {
            return func(old, arg);
        }
    };
};

template <class StorageF>
class input_pools
{
private:
    StorageF storage_;

public:
    input_pools() = default;

    explicit input_pools(StorageF storage)
        : storage_{std::move(storage)}
    {
    }

    auto& storage() { return storage_(); }
    const auto& storage() const { return storage_(); }

    template <class Container>
    const auto& get_pool()
    {
        using Contains =
            decltype(hana::contains(storage(), hana::type_c<Container>));
        if constexpr (!Contains::value) {
            auto err = error_missing_pool_for_type<Container>{};
        }
        return storage()[hana::type_c<Container>];
    }

    template <class OldContainer>
    auto& get_loader_by_old_container()
    {
        constexpr auto find_key = [](const auto& storage) {
            return hana::find_if(hana::keys(storage), [&](auto key) {
                using type1 = typename std::decay_t<
                    decltype(storage[key])>::old_container_t;
                return hana::type_c<type1> == hana::type_c<OldContainer>;
            });
        };
        using Key    = decltype(find_key(storage()));
        using IsJust = decltype(hana::is_just(Key{}));
        if constexpr (!IsJust::value) {
            auto err = error_missing_pool_for_type<OldContainer>{};
        }
        return storage()[Key{}.value()].get_loader_from_per_type_pool();
    }

    template <class Archive>
    void load(Archive& ar)
    {
        using pool_name_fn = typename Archive::pool_name_fn;
        using keys_t       = decltype(hana::keys(storage()));
        hana::for_each(keys_t{}, [&](auto key) {
            using Container  = typename decltype(key)::type;
            const auto& name = pool_name_fn{}(Container{});
            ar(cereal::make_nvp(name, storage()[key].pool));
        });
    }

    friend bool operator==(const input_pools& left, const input_pools& right)
    {
        return left.storage() == right.storage();
    }

    /**
     * ConversionMap is a map where keys are types of the original container
     * (hana::type_c<vector_one<model::snapshot>>) and the values are converting
     * functions that are used to convert the pools.
     *
     * The main feature is that the converting function can also take the second
     * argument, a function get_loader, which can be called with a container
     * type (`get_loader(hana::type_c<vector_one<format::track_id>>)`) and will
     * return a reference to a loader that can be used to load other containers
     * that have already been converted.
     *
     * @see test/extra/persist/test_conversion.cpp
     */
    template <class ConversionMap, class GetIdF>
    auto transform_recursive(const ConversionMap& conversion_map,
                             const GetIdF& get_id) const
    {
        // This lambda is only used to determine types and should never be
        // called.
        constexpr auto fake_convert_container = [](auto new_type,
                                                   const auto& old_container) {
            using NewContainerType = typename decltype(new_type)::type;
            throw std::runtime_error{"This should never be called"};
            return NewContainerType{};
        };

        // Return a pair where second is an optional. Optional is already
        // populated if no transformation is required.
        const auto transform_pair_initial = [fake_convert_container,
                                             &conversion_map,
                                             this](const auto& pair) {
            using Contains =
                decltype(hana::contains(conversion_map, hana::first(pair)));
            if constexpr (Contains::value) {
                // Look up the conversion function by the type from the
                // original pool.
                const auto& func = inject_argument(
                    fake_convert_container, conversion_map[hana::first(pair)]);
                auto type_load =
                    storage()[hana::first(pair)].with_transform(func);
                using NewContainer = typename decltype(type_load)::container_t;
                return hana::make_pair(hana::type_c<NewContainer>,
                                       std::move(type_load));
            } else {
                // If the conversion map doesn't mention the current type,
                // we leave it as is.
                return pair;
            }
        };

        // I can't think of a better way yet to tie all the loaders/transforming
        // functions together.
        auto shared_storage = [&] {
            auto new_storage = hana::fold_left(
                storage(),
                hana::make_map(),
                [transform_pair_initial](auto map, auto pair) {
                    return hana::insert(map, transform_pair_initial(pair));
                });
            using NewStorage = decltype(new_storage);
            return std::make_shared<NewStorage>(new_storage);
        }();

        using NewStorage = std::decay_t<decltype(*shared_storage)>;

        const auto convert_container = [get_id](auto get_data) {
            return [get_id, get_data](auto new_type,
                                      const auto& old_container) {
                const auto id  = get_id(old_container);
                using Contains = decltype(hana::contains(get_data(), new_type));
                if constexpr (!Contains::value) {
                    auto err = error_missing_pool_for_type<
                        typename decltype(new_type)::type>{};
                }
                auto& loader =
                    get_data()[new_type].get_loader_from_per_type_pool();
                return loader.load(id);
            };
        };

        // Important not to create a recursive reference to itself inside of the
        // shared_storage.
        const auto weak          = std::weak_ptr<NewStorage>{shared_storage};
        const auto get_data_weak = [weak]() -> auto& {
            auto p = weak.lock();
            if (!p) {
                throw std::logic_error{"weak ptr has expired"};
            }
            return *p;
        };

        // Fill-in the transforming functions into shared_storage.
        hana::for_each(hana::keys(*shared_storage), [&](auto key) {
            using TypeLoad = std::decay_t<decltype((*shared_storage)[key])>;
            const auto old_key =
                hana::type_c<typename TypeLoad::old_container_t>;
            using needs_conversion_t =
                decltype(hana::contains(conversion_map, old_key));
            if constexpr (needs_conversion_t::value) {
                const auto& old_func = conversion_map[old_key];
                (*shared_storage)[key].transform =
                    inject_argument(convert_container(get_data_weak), old_func);
            }
        });

        auto holder = make_shared_storage_holder(std::move(shared_storage));
        return input_pools<decltype(holder)>{std::move(holder)};
    }

    void merge_previous(const input_pools& original)
    {
        auto& s                = storage();
        const auto& original_s = original.storage();
        hana::for_each(hana::keys(s), [&](auto key) {
            s[key].merge_previous(original_s[key]);
        });
    }

    static auto generate_loaders()
    {
        using Storage = std::decay_t<decltype(std::declval<StorageF>()())>;
        using Types   = decltype(hana::keys(std::declval<Storage>()));
        auto storage =
            hana::fold_left(Types{}, hana::make_map(), [](auto map, auto type) {
                using TypePool =
                    std::decay_t<decltype(std::declval<Storage>()[type])>;
                return hana::insert(
                    map, hana::make_pair(type, TypePool::generate_loader()));
            });
        return storage;
    }
};

template <class T>
auto generate_output_pools(T types)
{
    auto storage =
        hana::fold_left(types, hana::make_map(), [](auto map, auto type) {
            using Type = typename decltype(type)::type;
            return hana::insert(
                map,
                hana::make_pair(
                    type, typename container_traits<Type>::output_pool_t{}));
        });

    using Storage = decltype(storage);
    return output_pools<Storage>{storage};
}

template <class T>
auto generate_input_pools(T types)
{
    auto storage =
        hana::fold_left(types, hana::make_map(), [](auto map, auto type) {
            using Type = typename decltype(type)::type;
            return hana::insert(map, hana::make_pair(type, input_pool<Type>{}));
        });

    auto storage_f = detail::make_storage_holder(std::move(storage));
    return input_pools{std::move(storage_f)};
}

template <class Storage>
inline auto to_input_pools(const output_pools<Storage>& output_pool)
{
    auto pool = generate_input_pools(boost::hana::keys(output_pool.storage()));
    boost::hana::for_each(boost::hana::keys(pool.storage()), [&](auto key) {
        pool.storage()[key].pool = to_input_pool(output_pool.storage()[key]);
    });
    return pool;
}

} // namespace immer::persist::detail
