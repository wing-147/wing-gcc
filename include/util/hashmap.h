#ifndef HASHMAP_H
#define HASHMAP_H

#include "util/common.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    HASHMAP_KEY_STRING_VIEW,
    HASHMAP_KEY_POINTER,
    HASHMAP_KEY_UINT64,
} HashMapKeyKind;

typedef struct HashEntry HashEntry;

// Initialize each table with hashmap_init() before use. Storage is private.
typedef struct HashMap {
    HashEntry *buckets;
    size_t capacity;
    size_t used; // Number of live buckets plus tombstones
    HashMapKeyKind key_kind;
} HashMap;

// Initialize a new or freed table with its key kind. Use the matching family
// of get, put and delete functions for every operation on that table.
void hashmap_init(HashMap *map, HashMapKeyKind key_kind);

// Byte-view keys are borrowed and compared by content. Their backing bytes
// must remain stable while stored. A lookup returns NULL for a missing key
// or a stored NULL value.
void *hashmap_get(const HashMap *map, StringView key);
void hashmap_put(HashMap *map, StringView key, void *value);
void hashmap_delete(HashMap *map, StringView key);

// Pointer keys are borrowed and compared by address, including NULL as a key.
// A lookup has the same missing-value convention as hashmap_get().
void *hashmap_get_pointer(const HashMap *map, const void *key);
void hashmap_put_pointer(HashMap *map, const void *key, void *value);
void hashmap_delete_pointer(HashMap *map, const void *key);

// Integer keys are stored by value and compared as full-width integers.
void *hashmap_get_uint64(const HashMap *map, uint64_t key);
void hashmap_put_uint64(HashMap *map, uint64_t key, void *value);
void hashmap_delete_uint64(HashMap *map, uint64_t key);

// Visit each occupied entry's value once, in unspecified order. The callback
// can free the value and must leave the mapping and its keys intact.
void hashmap_foreach_value(const HashMap *map, void (*visit)(void *value));

// Release the buckets, preserving the selected key kind for reuse. Borrowed
// keys and values remain owned by the caller.
void hashmap_free(HashMap *map);

#endif // HASHMAP_H
