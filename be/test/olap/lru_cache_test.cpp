// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/lru_cache.h"

#include <gtest/gtest.h>

#include <vector>

#include "test_util/test_util.h"
#include "util/logging.h"

using namespace doris;
using namespace std;

namespace doris {

void PutFixed32(std::string* dst, uint32_t value) {
    char buf[sizeof(value)];
    memcpy(buf, &value, sizeof(value));
    dst->append(buf, sizeof(buf));
}

uint32_t DecodeFixed32(const char* ptr) {
    // Load the raw bytes
    uint32_t result;
    memcpy(&result, ptr, sizeof(result)); // gcc optimizes this to a plain load
    return result;
}

// Conversions between numeric keys/values and the types expected by Cache.
const CacheKey EncodeKey(std::string* result, int k) {
    PutFixed32(result, k);
    return CacheKey(result->c_str(), result->size());
}

static int DecodeKey(const CacheKey& k) {
    assert(k.size() == 4);
    return DecodeFixed32(k.data());
}
static void* EncodeValue(uintptr_t v) {
    return reinterpret_cast<void*>(v);
}
static int DecodeValue(void* v) {
    return reinterpret_cast<uintptr_t>(v);
}

class CacheTest : public testing::Test {
public:
    static CacheTest* _s_current;

    static void Deleter(const CacheKey& key, void* v) {
        _s_current->_deleted_keys.push_back(DecodeKey(key));
        _s_current->_deleted_values.push_back(DecodeValue(v));
    }

    // there is 16 shards in ShardedLRUCache
    // And the LRUHandle size is about 100B. So the cache size should big enough
    // to run the UT.
    static const int kCacheSize = 1000 * 16;
    std::vector<int> _deleted_keys;
    std::vector<int> _deleted_values;
    Cache* _cache;

    CacheTest() : _cache(new_lru_cache("test", kCacheSize)) { _s_current = this; }

    ~CacheTest() { delete _cache; }

    int Lookup(int key) {
        std::string result;
        Cache::Handle* handle = _cache->lookup(EncodeKey(&result, key));
        const int r = (handle == nullptr) ? -1 : DecodeValue(_cache->value(handle));

        if (handle != nullptr) {
            _cache->release(handle);
        }

        return r;
    }

    void Insert(int key, int value, int charge) {
        std::string result;
        _cache->release(_cache->insert(EncodeKey(&result, key), EncodeValue(value), charge,
                                       &CacheTest::Deleter));
    }

    void InsertDurable(int key, int value, int charge) {
        std::string result;
        _cache->release(_cache->insert(EncodeKey(&result, key), EncodeValue(value), charge,
                                       &CacheTest::Deleter, CachePriority::DURABLE));
    }

    void Erase(int key) {
        std::string result;
        _cache->erase(EncodeKey(&result, key));
    }

    void SetUp() {}

    void TearDown() {}
};
CacheTest* CacheTest::_s_current;

TEST_F(CacheTest, HitAndMiss) {
    ASSERT_EQ(-1, Lookup(100));

    Insert(100, 101, 1);
    ASSERT_EQ(101, Lookup(100));
    ASSERT_EQ(-1, Lookup(200));
    ASSERT_EQ(-1, Lookup(300));

    Insert(200, 201, 1);
    ASSERT_EQ(101, Lookup(100));
    ASSERT_EQ(201, Lookup(200));
    ASSERT_EQ(-1, Lookup(300));

    Insert(100, 102, 1);
    ASSERT_EQ(102, Lookup(100));
    ASSERT_EQ(201, Lookup(200));
    ASSERT_EQ(-1, Lookup(300));

    ASSERT_EQ(1, _deleted_keys.size());
    ASSERT_EQ(100, _deleted_keys[0]);
    ASSERT_EQ(101, _deleted_values[0]);
}

TEST_F(CacheTest, Erase) {
    Erase(200);
    ASSERT_EQ(0, _deleted_keys.size());

    Insert(100, 101, 1);
    Insert(200, 201, 1);
    Erase(100);
    ASSERT_EQ(-1, Lookup(100));
    ASSERT_EQ(201, Lookup(200));
    ASSERT_EQ(1, _deleted_keys.size());
    ASSERT_EQ(100, _deleted_keys[0]);
    ASSERT_EQ(101, _deleted_values[0]);

    Erase(100);
    ASSERT_EQ(-1, Lookup(100));
    ASSERT_EQ(201, Lookup(200));
    ASSERT_EQ(1, _deleted_keys.size());
}

TEST_F(CacheTest, EntriesArePinned) {
    Insert(100, 101, 1);
    std::string result1;
    Cache::Handle* h1 = _cache->lookup(EncodeKey(&result1, 100));
    if (h1 == nullptr) {
        std::cout << "h1 is null" << std::endl;
    }
    ASSERT_EQ(101, DecodeValue(_cache->value(h1)));

    Insert(100, 102, 1);
    std::string result2;
    Cache::Handle* h2 = _cache->lookup(EncodeKey(&result2, 100));
    ASSERT_EQ(102, DecodeValue(_cache->value(h2)));
    ASSERT_EQ(0, _deleted_keys.size());

    _cache->release(h1);
    ASSERT_EQ(1, _deleted_keys.size());
    ASSERT_EQ(100, _deleted_keys[0]);
    ASSERT_EQ(101, _deleted_values[0]);

    Erase(100);
    ASSERT_EQ(-1, Lookup(100));
    ASSERT_EQ(1, _deleted_keys.size());

    _cache->release(h2);
    ASSERT_EQ(2, _deleted_keys.size());
    ASSERT_EQ(100, _deleted_keys[1]);
    ASSERT_EQ(102, _deleted_values[1]);
}

TEST_F(CacheTest, EvictionPolicy) {
    Insert(100, 101, 1);
    Insert(200, 201, 1);

    // Frequently used entry must be kept around
    for (int i = 0; i < kCacheSize + 100; i++) {
        Insert(1000 + i, 2000 + i, 1);
        ASSERT_EQ(2000 + i, Lookup(1000 + i));
        ASSERT_EQ(101, Lookup(100));
    }

    ASSERT_EQ(101, Lookup(100));
    ASSERT_EQ(-1, Lookup(200));
}

TEST_F(CacheTest, EvictionPolicyWithDurable) {
    Insert(100, 101, 1);
    InsertDurable(200, 201, 1);
    Insert(300, 101, 1);

    // Frequently used entry must be kept around
    for (int i = 0; i < kCacheSize + 100; i++) {
        Insert(1000 + i, 2000 + i, 1);
        ASSERT_EQ(2000 + i, Lookup(1000 + i));
        ASSERT_EQ(101, Lookup(100));
    }

    ASSERT_EQ(-1, Lookup(300));
    ASSERT_EQ(101, Lookup(100));
    ASSERT_EQ(201, Lookup(200));
}

static void deleter(const CacheKey& key, void* v) {
    std::cout << "delete key " << key.to_string() << std::endl;
}

static void insert_LRUCache(LRUCache& cache, const CacheKey& key, int value,
                            CachePriority priority) {
    uint32_t hash = key.hash(key.data(), key.size(), 0);
    static std::unique_ptr<MemTrackerLimiter> lru_cache_tracker =
        std::make_unique<MemTrackerLimiter>(-1, "TestLruCache");
    cache.release(cache.insert(key, hash, EncodeValue(value), value, &deleter,
                               lru_cache_tracker.get(), priority));
}

TEST_F(CacheTest, Usage) {
    LRUCache cache(LRUCacheType::SIZE);
    cache.set_capacity(1200);

    // The lru usage is handle_size + charge = 104 - 1 = 103
    // 103 + 3 means handle_size + key size
    CacheKey key1("100");
    insert_LRUCache(cache, key1, 100, CachePriority::NORMAL);
    ASSERT_EQ(206, cache.get_usage());

    CacheKey key2("200");
    insert_LRUCache(cache, key2, 200, CachePriority::DURABLE);
    ASSERT_EQ(512, cache.get_usage());

    CacheKey key3("300");
    insert_LRUCache(cache, key3, 300, CachePriority::NORMAL);
    ASSERT_EQ(918, cache.get_usage());

    CacheKey key4("400");
    insert_LRUCache(cache, key4, 400, CachePriority::NORMAL);
    ASSERT_EQ(812, cache.get_usage());

    CacheKey key5("500");
    insert_LRUCache(cache, key5, 500, CachePriority::NORMAL);
    ASSERT_EQ(912, cache.get_usage());

    CacheKey key6("600");
    insert_LRUCache(cache, key6, 600, CachePriority::NORMAL);
    ASSERT_EQ(1012, cache.get_usage());

    CacheKey key7("950");
    insert_LRUCache(cache, key7, 950, CachePriority::DURABLE);
    ASSERT_EQ(1056, cache.get_usage());
}

TEST_F(CacheTest, Prune) {
    LRUCache cache(LRUCacheType::NUMBER);
    cache.set_capacity(5);

    // The lru usage is handle_size + charge = 96 - 1 = 95
    // 95 + 3 means handle_size + key size
    CacheKey key1("100");
    insert_LRUCache(cache, key1, 100, CachePriority::NORMAL);
    ASSERT_EQ(1, cache.get_usage());

    CacheKey key2("200");
    insert_LRUCache(cache, key2, 200, CachePriority::DURABLE);
    ASSERT_EQ(2, cache.get_usage());

    CacheKey key3("300");
    insert_LRUCache(cache, key3, 300, CachePriority::NORMAL);
    ASSERT_EQ(3, cache.get_usage());

    CacheKey key4("400");
    insert_LRUCache(cache, key4, 400, CachePriority::NORMAL);
    ASSERT_EQ(4, cache.get_usage());

    CacheKey key5("500");
    insert_LRUCache(cache, key5, 500, CachePriority::NORMAL);
    ASSERT_EQ(5, cache.get_usage());

    CacheKey key6("600");
    insert_LRUCache(cache, key6, 600, CachePriority::NORMAL);
    ASSERT_EQ(5, cache.get_usage());

    CacheKey key7("700");
    insert_LRUCache(cache, key7, 700, CachePriority::DURABLE);
    ASSERT_EQ(5, cache.get_usage());

    auto pred = [](const void* value) -> bool { return false; };
    cache.prune_if(pred);
    ASSERT_EQ(5, cache.get_usage());

    auto pred2 = [](const void* value) -> bool { return DecodeValue((void*)value) > 400; };
    cache.prune_if(pred2);
    ASSERT_EQ(2, cache.get_usage());

    cache.prune();
    ASSERT_EQ(0, cache.get_usage());

    for (int i = 1; i <= 5; ++i) {
        insert_LRUCache(cache, CacheKey {std::to_string(i)}, i, CachePriority::NORMAL);
        ASSERT_EQ(i, cache.get_usage());
    }
    cache.prune_if([](const void*) { return true; });
    ASSERT_EQ(0, cache.get_usage());
}

TEST_F(CacheTest, HeavyEntries) {
    // Add a bunch of light and heavy entries and then count the combined
    // size of items still in the cache, which must be approximately the
    // same as the total capacity.
    const int kLight = 1;
    const int kHeavy = 10;
    int added = 0;
    int index = 0;

    while (added < 2 * kCacheSize) {
        const int weight = (index & 1) ? kLight : kHeavy;
        Insert(index, 1000 + index, weight);
        added += weight;
        index++;
    }

    int cached_weight = 0;

    for (int i = 0; i < index; i++) {
        const int weight = (i & 1 ? kLight : kHeavy);
        int r = Lookup(i);

        if (r >= 0) {
            cached_weight += weight;
            ASSERT_EQ(1000 + i, r);
        }
    }

    ASSERT_LE(cached_weight, kCacheSize + kCacheSize / 10);
}

TEST_F(CacheTest, NewId) {
    uint64_t a = _cache->new_id();
    uint64_t b = _cache->new_id();
    ASSERT_NE(a, b);
}

TEST_F(CacheTest, SimpleBenchmark) {
    for (int i = 0; i < kCacheSize * LOOP_LESS_OR_MORE(10, 10000); i++) {
        Insert(1000 + i, 2000 + i, 1);
        ASSERT_EQ(2000 + i, Lookup(1000 + i));
    }
}

TEST(CacheHandleTest, HandleTableTest) {
    HandleTable ht;

    for (uint32_t i = 0; i < ht._length; ++i) {
        ASSERT_NE(ht._list[i], nullptr);
        ASSERT_EQ(ht._list[i]->next_hash, nullptr);
        ASSERT_EQ(ht._list[i]->prev_hash, nullptr);
    }

    const int count = 10;
    CacheKey keys[count] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    ASSERT_NE(keys[0], keys[1]);
    LRUHandle* hs[count];
    for (int i = 0; i < count; ++i) {
        CacheKey* key = &keys[i];
        LRUHandle* h = reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key->size()));
        h->value = nullptr;
        h->deleter = nullptr;
        h->charge = 1;
        h->total_size = sizeof(LRUHandle) - 1 + key->size() + 1;
        h->key_length = key->size();
        h->hash = 1; // make them in a same hash table linked-list
        h->refs = 0;
        h->next = h->prev = nullptr;
        h->prev_hash = nullptr;
        h->next_hash = nullptr;
        h->in_cache = false;
        h->priority = CachePriority::NORMAL;
        memcpy(h->key_data, key->data(), key->size());

        LRUHandle* old = ht.insert(h);
        ASSERT_EQ(ht._elems, i + 1);
        ASSERT_EQ(old, nullptr); // there is no entry with the same key and hash
        hs[i] = h;
    }
    ASSERT_EQ(ht._elems, count);
    LRUHandle* h = ht.lookup(CacheKey(std::to_string(count - 1)), 1);
    LRUHandle* head = ht._list[1 & (ht._length - 1)];
    ASSERT_EQ(head, h->prev_hash);
    ASSERT_EQ(head->next_hash, h);
    int index = count - 1;
    while (h != nullptr) {
        ASSERT_EQ(hs[index], h) << index;
        h = h->next_hash;
        if (h != nullptr) {
            ASSERT_EQ(hs[index], h->prev_hash);
        }
        --index;
    }

    for (int i = 0; i < count; ++i) {
        CacheKey* key = &keys[i];
        LRUHandle* h = reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key->size()));
        h->value = nullptr;
        h->deleter = nullptr;
        h->charge = 1;
        h->total_size = sizeof(LRUHandle) - 1 + key->size() + 1;
        h->key_length = key->size();
        h->hash = 1; // make them in a same hash table linked-list
        h->refs = 0;
        h->next = h->prev = nullptr;
        h->prev_hash = nullptr;
        h->next_hash = nullptr;
        h->in_cache = false;
        h->priority = CachePriority::NORMAL;
        memcpy(h->key_data, key->data(), key->size());

        ASSERT_EQ(ht.insert(h), hs[i]); // there is an entry with the same key and hash
        ASSERT_EQ(ht._elems, count);
        free(hs[i]);
        hs[i] = h;
    }
    ASSERT_EQ(ht._elems, count);

    for (int i = 0; i < count; ++i) {
        ASSERT_EQ(ht.lookup(keys[i], 1), hs[i]);
    }

    LRUHandle* old = ht.remove(CacheKey("9"), 1); // first in hash table linked-list
    ASSERT_EQ(old, hs[9]);
    ASSERT_EQ(old->prev_hash, head);
    ASSERT_EQ(old->next_hash, hs[8]); // hs[8] is the new first node
    ASSERT_EQ(head->next_hash, hs[8]);
    ASSERT_EQ(hs[8]->prev_hash, head);

    old = ht.remove(CacheKey("0"), 1); // last in hash table linked-list
    ASSERT_EQ(old, hs[0]);
    ASSERT_EQ(old->prev_hash, hs[1]); // hs[1] is the new last node
    ASSERT_EQ(old->prev_hash->next_hash, nullptr);

    old = ht.remove(CacheKey("5"), 1); // middle in hash table linked-list
    ASSERT_EQ(old, hs[5]);
    ASSERT_EQ(old->prev_hash, hs[6]);
    ASSERT_EQ(old->next_hash, hs[4]);
    ASSERT_EQ(hs[6]->next_hash, hs[4]);
    ASSERT_EQ(hs[4]->prev_hash, hs[6]);

    ht.remove(hs[4]); // middle in hash table linked-list
    ASSERT_EQ(hs[6]->next_hash, hs[3]);
    ASSERT_EQ(hs[3]->prev_hash, hs[6]);

    ASSERT_EQ(ht._elems, count - 4);

    for (int i = 0; i < count; ++i) {
        free(hs[i]);
    }
}

} // namespace doris

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
