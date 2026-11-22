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
#define TOMBSTONE ((const char *)-1)

// Calculate hash value of a key using the FNV-1 algorithm.
static uint64_t fnv_hash(StringView key)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < string_view_length(key); i++) {
        hash *= 0x100000001b3;
        hash ^= (unsigned char)string_view_begin(key)[i];
    }
    return hash;
}

// Check whether a hash entry matches the given key.
static bool match(const HashEntry *ent, StringView key)
{
    return string_view_begin(ent->key) && string_view_begin(ent->key) != TOMBSTONE &&
           string_view_equal(ent->key, key);
}

// Find an existing entry with the specified key.
static HashEntry *find_entry(const HashMap *map, StringView key)
{
    if (!map->buckets) {
        return NULL;
    }

    uint64_t hash = fnv_hash(key);

    for (size_t i = 0; i < map->capacity; i++) {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        if (string_view_begin(ent->key) == NULL) {
            return NULL;
        }
        if (match(ent, key)) {
            return ent;
        }
    }
    unreachable();
}

// Find an existing entry by key or insert a new entry into the hashmap.
// A tombstone is only reused after the rest of the probe chain has been
// searched: a live entry for the same key may sit behind it, and inserting in
// front would create a duplicate that deletion leaves behind as a stale value.
static HashEntry *find_or_create_entry(HashMap *map, StringView key)
{
    uint64_t hash = fnv_hash(key);
    HashEntry *tombstone = NULL;

    for (size_t i = 0; i < map->capacity; i++) {
        HashEntry *ent = &map->buckets[(hash + i) % map->capacity];
        if (match(ent, key)) {
            return ent;
        }
        if (string_view_begin(ent->key) == TOMBSTONE) {
            if (!tombstone) {
                tombstone = ent;
            }
        } else if (string_view_begin(ent->key) == NULL) {
            if (tombstone) {
                tombstone->key = key;
                return tombstone;
            }
            ent->key = key;
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
        if (string_view_begin(map->buckets[i].key) &&
            string_view_begin(map->buckets[i].key) != TOMBSTONE) {
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
        if (string_view_begin(ent->key) && string_view_begin(ent->key) != TOMBSTONE) {
            HashEntry *ent2 = find_or_create_entry(&tmp, ent->key);
            ent2->val = ent->val;
        }
    }

    free(map->buckets);
    *map = tmp;
}

// Get the value associated with one borrowed byte-view key.
void *hashmap_get(const HashMap *map, StringView key)
{
    HashEntry *ent = find_entry(map, key);
    return ent ? ent->val : NULL;
}

// Insert or replace one borrowed byte-view key and caller-provided value.
void hashmap_put(HashMap *map, StringView key, void *value)
{
    if (!map->buckets) {
        map->buckets = xcalloc(INITIAL_CAPACITY, sizeof(HashEntry));
        map->capacity = INITIAL_CAPACITY;
    } else if ((map->used * 100) / map->capacity >= MAX_LOAD_FACTOR) {
        hashmap_rehash(map);
    }

    HashEntry *ent = find_or_create_entry(map, key);
    ent->val = value;
}

// Delete the value associated with one borrowed byte-view key.
void hashmap_delete(HashMap *map, StringView key)
{
    HashEntry *ent = find_entry(map, key);
    if (ent)
        ent->key = string_view(TOMBSTONE, 0);
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
        hashmap_put(&map, string_view_from_cstr(keys[i]), (void *)(size_t)i);

    for (int i = 1000; i < 2000; i++)
        hashmap_delete(&map, string_view_from_cstr(keys[i]));

    for (int i = 1500; i < 1600; i++)
        hashmap_put(&map, string_view_from_cstr(keys[i]), (void *)(size_t)i);

    for (int i = 6000; i < 7000; i++)
        hashmap_put(&map, string_view_from_cstr(keys[i]), (void *)(size_t)i);

    for (int i = 0; i < 1000; i++)
        assert((size_t)hashmap_get(&map, string_view_from_cstr(keys[i])) ==
               (size_t)i);

    for (int i = 1000; i < 1500; i++)
        assert(hashmap_get(&map, string_view_from_cstr(keys[i])) == NULL);

    for (int i = 1500; i < 1600; i++)
        assert((size_t)hashmap_get(&map, string_view_from_cstr(keys[i])) ==
               (size_t)i);

    for (int i = 1600; i < 2000; i++)
        assert(hashmap_get(&map, string_view_from_cstr(keys[i])) == NULL);

    for (int i = 2000; i < 5000; i++)
        assert((size_t)hashmap_get(&map, string_view_from_cstr(keys[i])) ==
               (size_t)i);

    for (int i = 5000; i < 6000; i++)
        assert(hashmap_get(&map, string_view_from_cstr(keys[i])) == NULL);

    for (int i = 6000; i < 7000; i++)
        assert((size_t)hashmap_get(&map, string_view_from_cstr(keys[i])) ==
               (size_t)i);

    assert(hashmap_get(&map, string_view_from_cstr("no such key")) == NULL);

    // "k9" and "k12" share the initial bucket, so k9's tombstone sits in front
    // of k12's live slot. Re-inserting k12 must update that slot instead of
    // adding a duplicate: otherwise deleting it would resurrect the old value.
    HashMap dup = { 0 };
    hashmap_put(&dup, string_view_from_cstr("k9"), (void *)(size_t)1);
    hashmap_put(&dup, string_view_from_cstr("k12"), (void *)(size_t)2);
    hashmap_delete(&dup, string_view_from_cstr("k9"));
    hashmap_put(&dup, string_view_from_cstr("k12"), (void *)(size_t)3);
    assert((size_t)hashmap_get(&dup, string_view_from_cstr("k12")) == 3);
    hashmap_delete(&dup, string_view_from_cstr("k12"));
    assert(hashmap_get(&dup, string_view_from_cstr("k12")) == NULL);
    hashmap_free(&dup);

    // A map key is an explicit byte view rather than a NUL-terminated string;
    // bytes after an embedded NUL remain part of its identity.
    char embedded_key[] = { 'a', '\0', 'b' };
    char short_key[] = { 'a' };
    HashMap views = { 0 };
    hashmap_put(&views, string_view(embedded_key, sizeof(embedded_key)),
                (void *)(size_t)7);
    assert((size_t)hashmap_get(
               &views, string_view(embedded_key, sizeof(embedded_key))) == 7);
    assert(hashmap_get(&views, string_view(short_key, sizeof(short_key))) ==
           NULL);
    hashmap_free(&views);

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
