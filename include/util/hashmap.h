#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>

typedef struct HashEntry {
    const char *key; // Key lifetime is managed by the caller
    size_t key_len;
    void *val; // User-provided value
} HashEntry;

typedef struct HashMap {
    HashEntry *buckets;
    size_t capacity;
    size_t used; // Number of occupied buckets, including tombstones
} HashMap;

// Look up a key. Returns NULL for a missing key; a stored NULL value is
// indistinguishable from a missing key, so callers must store non-NULL values.
void *hashmap_get_range(const HashMap *map, const char *key, size_t key_len);
void *hashmap_get(const HashMap *map, const char *key);
void hashmap_put_range(HashMap *map, const char *key, size_t key_len, void *value);
void hashmap_put(HashMap *map, const char *key, void *value);
void hashmap_delete_range(HashMap *map, const char *key, size_t key_len);
void hashmap_delete(HashMap *map, const char *key);

// Release the bucket array. Keys and values are owned by the caller.
void hashmap_free(HashMap *map);

#endif // HASHMAP_H
