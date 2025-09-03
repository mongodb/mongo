#ifndef LRU_CACHE_HPP
#define LRU_CACHE_HPP

#include "utils/error.hpp"
#include "utils/optional.hpp"

#include <list>
#include <unordered_map>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    template<typename K, typename V>
    class lru_cache {
        struct kvp {
            K key;
            V value;
        };
        using list_type = std::list<kvp>;
        using list_iterator = typename list_type::iterator;
        mutable list_type lru;
        std::unordered_map<K, list_iterator> map;
        optional<std::size_t> max_size;

    public:
        lru_cache() = default;
        lru_cache(optional<std::size_t> max_size) : max_size(max_size) {
            VERIFY(!max_size || max_size.unwrap() > 0);
        }

        void set_max_size(optional<std::size_t> size) {
            VERIFY(!size || size.unwrap() > 0);
            max_size = size;
            maybe_trim();
        }

        void maybe_touch(const K& key) {
            auto it = map.find(key);
            if(it == map.end()) {
                return;
            }
            auto list_it = it->second;
            touch(list_it);
        }

        optional<V&> maybe_get(const K& key) {
            auto it = map.find(key);
            if(it == map.end()) {
                return nullopt;
            } else {
                touch(it->second);
                return it->second->value;
            }
        }

        optional<const V&> maybe_get(const K& key) const {
            auto it = map.find(key);
            if(it == map.end()) {
                return nullopt;
            } else {
                touch(it->second);
                return it->second->value;
            }
        }

        void set(const K& key, V value) {
            auto it = map.find(key);
            if(it == map.end()) {
                insert(key, std::move(value));
            } else {
                touch(it->second);
                it->second->value = std::move(value);
            }
        }

        optional<V&> insert(const K& key, V value) {
            auto pair = map.insert({key, lru.end()});
            if(!pair.second) {
                // didn't insert
                return nullopt;
            }
            auto map_it = pair.first;
            lru.push_front({key, std::move(value)});
            map_it->second = lru.begin();
            maybe_trim();
            return lru.front().value;
        }

        std::size_t size() const {
            return lru.size();
        }

    private:
        void touch(list_iterator list_it) const {
            lru.splice(lru.begin(), lru, list_it);
        }

        void maybe_trim() {
            while(max_size && lru.size() > max_size.unwrap()) {
                const auto& to_remove = lru.back();
                map.erase(to_remove.key);
                lru.pop_back();
            }
        }
    };
}
CPPTRACE_END_NAMESPACE

#endif
