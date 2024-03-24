#pragma once
#include <map>
#include <vector>
#include <mutex>

using namespace std::string_literals;

template <typename Key, typename Value>
class ConcurrentMap {
private:
    struct Part;
public:
    static_assert(std::is_integral_v<Key>, "ConcurrentMap supports only integer keys"s);

    struct Access { 
        std::lock_guard<std::mutex> guard;
        Value& ref_to_value;

        Access(const Key& key, Part& part)
            :guard(part.mutex)
            , ref_to_value(part.map[key]) 
        {
        }
    };

    explicit ConcurrentMap(size_t part_count)
        :parts_(part_count)
    {
    }

    Access operator[](const Key& key) {
        auto& part = parts_[static_cast<uint64_t>(key) % parts_.size()];
        return { key, part };
    }

    std::map<Key, Value> BuildOrdinaryMap() {
        std::map<Key, Value> result;
        for (auto& [mutex, map] : parts_) {
            std::lock_guard guard(mutex);
            result.insert(map.begin(), map.end());
        }
        return result;
    }
    
    void Remove(const Key& key) {
       uint64_t part = static_cast<uint64_t>(key % parts_.size());
       std::lock_guard guard(parts_[part].mutex);
       parts_[part].map.erase(key);
    }

private:
    struct Part{
        std::mutex mutex;
        std::map<Key, Value> map;
    };
    std::vector<Part> parts_;
};
