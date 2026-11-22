#ifndef HASHMAP_H
#define HASHMAP_H

#include "util/common.h"

#include <stddef.h>

typedef struct HashEntry {
    StringView key; // Borrowed; key storage is managed by the caller
    void *val; // User-provided value
} HashEntry;

typedef struct HashMap {
    HashEntry *buckets;
    size_t capacity;
    size_t used; // Number of occupied buckets, including tombstones
} HashMap;

// Look up a byte-view key. Returns NULL for a missing key; a stored NULL value
// is indistinguishable from a missing key, so callers store non-NULL values.
void *hashmap_get(const HashMap *map, StringView key);
void hashmap_put(HashMap *map, StringView key, void *value);
void hashmap_delete(HashMap *map, StringView key);

// Release the bucket array. Keys and values are owned by the caller.
void hashmap_free(HashMap *map);

#endif // HASHMAP_H
