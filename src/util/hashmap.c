// The keyword table is read far more often than it is written, so the map uses
// open addressing with tombstones instead of chaining; keys and values stay
// owned by the caller.

#include "util/hashmap.h"
#include "util/common.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16
// Rehash when load factor exceeds this threshold.
#define MAX_LOAD_FACTOR 70

// Keep load factor below this value after rehashing.
#define TARGET_LOAD_FACTOR 50

// Capacity grows by this factor during rehashing.
#define CAPACITY_GROWTH_FACTOR 2

// Represents a deleted hash entry
#define TOMBSTONE ((void *)-1)

// Calculate hash value of a key using the FNV-1 algorithm.
static uint64_t fnv_hash(const char *s, size_t len)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < len; i++) {
        hash *= 0x100000001b3;
        hash ^= (unsigned char)s[i];
    }
    return hash;
}

// Check whether a hash entry matches the given key.
static bool match(const HashEntry *ent, const char *key, size_t key_len)
{
    return ent->key && ent->key != TOMBSTONE && ent->key_len == key_len &&
           !memcmp(ent->key, key, key_len);
}

// Find an existing entry with the specified key.
static HashEntry *find_entry(const HashMap *map, const char *key, size_t key_len)
{
    if (!map->buckets) {
        return NULL;
    }

    uint64_t hash = fnv_hash(key, key_len);

    for (size_t i = 0; i < map->capacity; i++) {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        if (ent->key == NULL) {
            return NULL;
        }
        if (match(ent, key, key_len)) {
            return ent;
        }
    }
    unreachable();
}

// Find an existing entry by key or insert a new entry into the hashmap.
// A tombstone is only reused after the rest of the probe chain has been
// searched: a live entry for the same key may sit behind it, and inserting in
// front would create a duplicate that deletion leaves behind as a stale value.
static HashEntry *find_or_create_entry(HashMap *map, const char *key, size_t key_len)
{
    uint64_t hash = fnv_hash(key, key_len);
    HashEntry *tombstone = NULL;

    for (size_t i = 0; i < map->capacity; i++) {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        if (match(ent, key, key_len)) {
            return ent;
        }
        if (ent->key == TOMBSTONE) {
            if (!tombstone) {
                tombstone = ent;
            }
        } else if (ent->key == NULL) {
            if (tombstone) {
                tombstone->key = key;
                tombstone->key_len = key_len;
                return tombstone;
            }
            ent->key = key;
            ent->key_len = key_len;
            map->used++;
            return ent;
        }
    }
    unreachable();
}

// Rebuild hashmap buckets, remove tombstones,
// and expand capacity when necessary.
static void hashmap_rehash(HashMap *map)
{
    size_t key_count = 0;
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE) {
            key_count++;
        }
    }

    size_t cap = map->capacity;
    while ((key_count * 100) / cap >= TARGET_LOAD_FACTOR) {
        cap *= CAPACITY_GROWTH_FACTOR;
    }

    HashMap tmp = { 0 };
    tmp.buckets = xcalloc(cap, sizeof(HashEntry));
    tmp.capacity = cap;

    for (size_t i = 0; i < map->capacity; i++) {
        HashEntry *ent = &map->buckets[i];
        if (ent->key && ent->key != TOMBSTONE) {
            HashEntry *ent2 = find_or_create_entry(&tmp, ent->key, ent->key_len);
            ent2->val = ent->val;
        }
    }

    free(map->buckets);
    *map = tmp;
}

// Get value associated with a key range.
// This version supports lexer tokens that are not null-terminated.
void *hashmap_get_range(const HashMap *map, const char *key, size_t key_len)
{
    HashEntry *ent = find_entry(map, key, key_len);
    return ent ? ent->val : NULL;
}

// Get value associated with a null-terminated string key.
void *hashmap_get(const HashMap *map, const char *key)
{
    return hashmap_get_range(map, key, strlen(key));
}

// Insert key-value pair using an explicit key length.
// Stores the association between key and user-provided value.
void hashmap_put_range(HashMap *map, const char *key, size_t key_len, void *value)
{
    if (!map->buckets) {
        map->buckets = xcalloc(INITIAL_CAPACITY, sizeof(HashEntry));
        map->capacity = INITIAL_CAPACITY;
    } else if ((map->used * 100) / map->capacity >= MAX_LOAD_FACTOR) {
        hashmap_rehash(map);
    }

    HashEntry *ent = find_or_create_entry(map, key, key_len);
    ent->val = value;
}

// Insert key-value pair using a null-terminated string key.
void hashmap_put(HashMap *map, const char *key, void *value)
{
    hashmap_put_range(map, key, strlen(key), value);
}

// Delete value associated with a key range.
void hashmap_delete_range(HashMap *map, const char *key, size_t key_len)
{
    HashEntry *ent = find_entry(map, key, key_len);
    if (ent) {
        ent->key = TOMBSTONE;
    }
}

// Delete value associated with a null-terminated string key.
void hashmap_delete(HashMap *map, const char *key)
{
    hashmap_delete_range(map, key, strlen(key));
}

// Release the bucket array. Keys and values are owned by the caller.
void hashmap_free(HashMap *map)
{
    free(map->buckets);
    map->buckets = NULL;
    map->capacity = 0;
    map->used = 0;
}

#ifdef TEST_HASHMAP
#include <assert.h>
#include <stdio.h>

// Test basic hashmap operations.
// Used during hashmap development.
void hashmap_test(void)
{
    HashMap map = { 0 };

    char **keys = calloc(7000, sizeof(char *));
    assert(keys != NULL);

    for (int i = 0; i < 7000; i++) {
        keys[i] = calloc(32, 1);
        assert(keys[i] != NULL);
        snprintf(keys[i], 32, "key %d", i);
    }

    for (int i = 0; i < 5000; i++)
        hashmap_put(&map, keys[i], (void *)(size_t)i);

    for (int i = 1000; i < 2000; i++)
        hashmap_delete(&map, keys[i]);

    for (int i = 1500; i < 1600; i++)
        hashmap_put(&map, keys[i], (void *)(size_t)i);

    for (int i = 6000; i < 7000; i++)
        hashmap_put(&map, keys[i], (void *)(size_t)i);

    for (int i = 0; i < 1000; i++)
        assert((size_t)hashmap_get(&map, keys[i]) == (size_t)i);

    for (int i = 1000; i < 1500; i++)
        assert(hashmap_get(&map, keys[i]) == NULL);

    for (int i = 1500; i < 1600; i++)
        assert((size_t)hashmap_get(&map, keys[i]) == (size_t)i);

    for (int i = 1600; i < 2000; i++)
        assert(hashmap_get(&map, keys[i]) == NULL);

    for (int i = 2000; i < 5000; i++)
        assert((size_t)hashmap_get(&map, keys[i]) == (size_t)i);

    for (int i = 5000; i < 6000; i++)
        assert(hashmap_get(&map, keys[i]) == NULL);

    for (int i = 6000; i < 7000; i++)
        assert((size_t)hashmap_get(&map, keys[i]) == (size_t)i);

    assert(hashmap_get(&map, "no such key") == NULL);

    // "k9" and "k12" share the initial bucket, so k9's tombstone sits in front
    // of k12's live slot. Re-inserting k12 must update that slot instead of
    // adding a duplicate: otherwise deleting it would resurrect the old value.
    HashMap dup = { 0 };
    hashmap_put(&dup, "k9", (void *)(size_t)1);
    hashmap_put(&dup, "k12", (void *)(size_t)2);
    hashmap_delete(&dup, "k9");
    hashmap_put(&dup, "k12", (void *)(size_t)3);
    assert((size_t)hashmap_get(&dup, "k12") == 3);
    hashmap_delete(&dup, "k12");
    assert(hashmap_get(&dup, "k12") == NULL);
    hashmap_free(&dup);

    hashmap_free(&map);

    printf("OK\n");

    for (int i = 0; i < 7000; i++)
        free(keys[i]);

    free(keys);
}

int main(void)
{
    hashmap_test();
    return 0;
}

#endif
