// All key kinds share one open-addressed table. Values and borrowed keys
// are owned by the caller.

#include "util/hashmap.h"

#include "util/common.h"

#include <stdint.h>
#include <stdlib.h>

#define INITIAL_CAPACITY 16
#define MAX_LOAD_PERCENT 70

typedef enum {
    ENTRY_EMPTY,
    ENTRY_OCCUPIED,
    ENTRY_DELETED,
} EntryState;

typedef union {
    StringView bytes;
    const void *pointer;
    uint64_t integer;
} HashKey;

struct HashEntry {
    HashKey key;
    void *value;
    EntryState state;
};

// Hash the complete byte range with FNV-1, including interior NUL bytes.
static uint64_t hash_bytes(StringView key)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < string_view_length(key); ++i) {
        hash *= 0x100000001b3;
        hash ^= (unsigned char)string_view_begin(key)[i];
    }
    return hash;
}

// Mix a numeric key so differences in high bits also affect low buckets.
static uint64_t hash_uint64(uint64_t hash)
{
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    return hash ^ (hash >> 33);
}

// Hash the key variant selected by the table's key kind.
static uint64_t hash_key(HashMapKeyKind kind, HashKey key)
{
    switch (kind) {
    case HASHMAP_KEY_STRING_VIEW: return hash_bytes(key.bytes);
    case HASHMAP_KEY_POINTER: return hash_uint64((uint64_t)(uintptr_t)key.pointer);
    case HASHMAP_KEY_UINT64: return hash_uint64(key.integer);
    }
    unreachable();
}

// Compare two keys using the table's fixed identity rule.
static bool keys_equal(HashMapKeyKind kind, HashKey lhs, HashKey rhs)
{
    switch (kind) {
    case HASHMAP_KEY_STRING_VIEW: return string_view_equal(lhs.bytes, rhs.bytes);
    case HASHMAP_KEY_POINTER: return lhs.pointer == rhs.pointer;
    case HASHMAP_KEY_UINT64: return lhs.integer == rhs.integer;
    }
    unreachable();
}

// Find a key or its insertion slot in an allocated, non-full table. Defer
// using the first tombstone until an empty slot proves that the key is absent.
static size_t find_entry_slot(const HashMap *map, HashKey key)
{
    size_t mask = map->capacity - 1;
    size_t first_deleted = map->capacity;
    size_t slot = (size_t)hash_key(map->key_kind, key) & mask;
    for (size_t i = 0; i < map->capacity; ++i) {
        const HashEntry *entry = &map->buckets[slot];
        switch (entry->state) {
        case ENTRY_EMPTY:
            return first_deleted == map->capacity ? slot : first_deleted;
        case ENTRY_OCCUPIED:
            if (keys_equal(map->key_kind, entry->key, key))
                return slot;
            break;
        case ENTRY_DELETED:
            if (first_deleted == map->capacity)
                first_deleted = slot;
            break;
        }
        slot = (slot + 1) & mask;
    }
    unreachable();
}

// Place a key without changing capacity. Replacing a value keeps the original
// borrowed key so a temporary equal key can be used to update a byte entry.
static void put_entry(HashMap *map, HashKey key, void *value)
{
    HashEntry *entry = &map->buckets[find_entry_slot(map, key)];
    if (entry->state != ENTRY_OCCUPIED) {
        if (entry->state == ENTRY_EMPTY)
            ++map->used;
        *entry = (HashEntry){ .key = key, .state = ENTRY_OCCUPIED };
    }
    entry->value = value;
}

// Rebuild without tombstones. Doubling keeps the live load below one half
// when the current table could no longer reach that load by compaction alone.
static void rebuild_entries(HashMap *map)
{
    size_t capacity = map->capacity;
    size_t live_entries = 0;
    for (size_t i = 0; i < capacity; ++i)
        live_entries += map->buckets[i].state == ENTRY_OCCUPIED;

    if (live_entries >= capacity / 2) {
        if (capacity > SIZE_MAX / 2)
            fatal("hash map capacity overflow");
        capacity *= 2;
    }

    HashMap rebuilt;
    hashmap_init(&rebuilt, map->key_kind);
    rebuilt.buckets = xcalloc(capacity, sizeof(HashEntry));
    rebuilt.capacity = capacity;
    for (size_t i = 0; i < map->capacity; ++i) {
        const HashEntry *entry = &map->buckets[i];
        if (entry->state == ENTRY_OCCUPIED)
            put_entry(&rebuilt, entry->key, entry->value);
    }
    free(map->buckets);
    *map = rebuilt;
}

// Compute the first occupied count that reaches the load limit without
// multiplying a potentially large capacity by 100.
static size_t load_limit(size_t capacity)
{
    return (capacity / 100) * MAX_LOAD_PERCENT +
           ((capacity % 100) * MAX_LOAD_PERCENT + 99) / 100;
}

// Read the value for an already typed key, or NULL when it is absent.
static void *get_key(const HashMap *map, HashKey key)
{
    if (!map->buckets)
        return NULL;
    const HashEntry *entry = &map->buckets[find_entry_slot(map, key)];
    return entry->state == ENTRY_OCCUPIED ? entry->value : NULL;
}

// Make room for an already typed key and store its borrowed value.
static void put_key(HashMap *map, HashKey key, void *value)
{
    if (!map->buckets) {
        map->buckets = xcalloc(INITIAL_CAPACITY, sizeof(HashEntry));
        map->capacity = INITIAL_CAPACITY;
    } else if (map->used >= load_limit(map->capacity)) {
        rebuild_entries(map);
    }
    put_entry(map, key, value);
}

// Remove an already typed key while keeping its probe chain traversable.
static void delete_key(HashMap *map, HashKey key)
{
    if (!map->buckets)
        return;
    HashEntry *entry = &map->buckets[find_entry_slot(map, key)];
    if (entry->state == ENTRY_OCCUPIED)
        *entry = (HashEntry){ .state = ENTRY_DELETED };
}

// Guard the table's single key interpretation before passing a typed key.
static void require_key_kind(const HashMap *map, HashMapKeyKind expected)
{
    if (map->key_kind != expected)
        unreachable();
}

// Initialize an empty table in the caller-selected key mode.
void hashmap_init(HashMap *map, HashMapKeyKind key_kind)
{
    *map = (HashMap){ .key_kind = key_kind };
}

// Read a value by borrowed byte content.
void *hashmap_get(const HashMap *map, StringView key)
{
    require_key_kind(map, HASHMAP_KEY_STRING_VIEW);
    return get_key(map, (HashKey){ .bytes = key });
}

// Store a borrowed byte key and caller-owned value.
void hashmap_put(HashMap *map, StringView key, void *value)
{
    require_key_kind(map, HASHMAP_KEY_STRING_VIEW);
    put_key(map, (HashKey){ .bytes = key }, value);
}

// Remove a mapping by borrowed byte content.
void hashmap_delete(HashMap *map, StringView key)
{
    require_key_kind(map, HASHMAP_KEY_STRING_VIEW);
    delete_key(map, (HashKey){ .bytes = key });
}

// Read a value by pointer identity.
void *hashmap_get_pointer(const HashMap *map, const void *key)
{
    require_key_kind(map, HASHMAP_KEY_POINTER);
    return get_key(map, (HashKey){ .pointer = key });
}

// Store a pointer key and caller-owned value without dereferencing the key.
void hashmap_put_pointer(HashMap *map, const void *key, void *value)
{
    require_key_kind(map, HASHMAP_KEY_POINTER);
    put_key(map, (HashKey){ .pointer = key }, value);
}

// Remove a mapping by pointer identity.
void hashmap_delete_pointer(HashMap *map, const void *key)
{
    require_key_kind(map, HASHMAP_KEY_POINTER);
    delete_key(map, (HashKey){ .pointer = key });
}

// Read a mapping by the full width of its integer key.
void *hashmap_get_uint64(const HashMap *map, uint64_t key)
{
    require_key_kind(map, HASHMAP_KEY_UINT64);
    return get_key(map, (HashKey){ .integer = key });
}

// Store an integer key by value with a caller-owned value.
void hashmap_put_uint64(HashMap *map, uint64_t key, void *value)
{
    require_key_kind(map, HASHMAP_KEY_UINT64);
    put_key(map, (HashKey){ .integer = key }, value);
}

// Remove a mapping by its integer key.
void hashmap_delete_uint64(HashMap *map, uint64_t key)
{
    require_key_kind(map, HASHMAP_KEY_UINT64);
    delete_key(map, (HashKey){ .integer = key });
}

// Visit the value of each live entry while leaving the index intact.
void hashmap_foreach_value(const HashMap *map, void (*visit)(void *value))
{
    for (size_t i = 0; i < map->capacity; ++i) {
        const HashEntry *entry = &map->buckets[i];
        if (entry->state == ENTRY_OCCUPIED)
            visit(entry->value);
    }
}

// Release only bucket storage, keeping the key kind for the next use.
void hashmap_free(HashMap *map)
{
    free(map->buckets);
    hashmap_init(map, map->key_kind);
}

#ifdef TEST_HASHMAP
#include <assert.h>
#include <stdio.h>

// Exercise byte keys through growth, replacement and deletion.
static void test_byte_keys(void)
{
    HashMap map;
    hashmap_init(&map, HASHMAP_KEY_STRING_VIEW);

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
    HashMap dup;
    hashmap_init(&dup, HASHMAP_KEY_STRING_VIEW);
    hashmap_put(&dup, string_view_from_cstr("k9"), (void *)(size_t)1);
    hashmap_put(&dup, string_view_from_cstr("k12"), (void *)(size_t)2);
    hashmap_delete(&dup, string_view_from_cstr("k9"));
    hashmap_put(&dup, string_view_from_cstr("k12"), (void *)(size_t)3);
    assert((size_t)hashmap_get(&dup, string_view_from_cstr("k12")) == 3);
    hashmap_delete(&dup, string_view_from_cstr("k12"));
    assert(hashmap_get(&dup, string_view_from_cstr("k12")) == NULL);
    hashmap_free(&dup);

    // Byte views compare all bytes, including after an embedded NUL.
    char embedded_key[] = { 'a', '\0', 'b' };
    char equal_key[] = { 'a', '\0', 'b' };
    char short_key[] = { 'a' };
    HashMap views;
    hashmap_init(&views, HASHMAP_KEY_STRING_VIEW);
    hashmap_put(&views, string_view(embedded_key, sizeof(embedded_key)),
                (void *)(size_t)7);
    assert((size_t)hashmap_get(
               &views, string_view(equal_key, sizeof(equal_key))) == 7);
    assert(hashmap_get(&views, string_view(short_key, sizeof(short_key))) ==
           NULL);
    hashmap_free(&views);

    hashmap_free(&map);

    for (int i = 0; i < 7000; i++)
        free(keys[i]);

    free(keys);
}

// Require the visitor to reach each supplied marker exactly once.
static void mark_visited(void *value)
{
    bool *visited = value;
    assert(!*visited);
    *visited = true;
}

// Cover address identity, growth through tombstones, visitation and reuse.
static void test_pointer_keys(void)
{
    enum { KEY_COUNT = 128, FIRST_BATCH = KEY_COUNT / 2 };
    int keys[KEY_COUNT] = {0};
    bool visited[KEY_COUNT] = {0};
    bool visited_null = false;
    HashMap map;
    hashmap_init(&map, HASHMAP_KEY_POINTER);
    assert(hashmap_get_pointer(&map, &keys[0]) == NULL);

    for (size_t i = 0; i < FIRST_BATCH; ++i)
        hashmap_put_pointer(&map, &keys[i], &visited[i]);
    assert(hashmap_get_pointer(&map, &keys[0]) == &visited[0]);
    assert(hashmap_get_pointer(&map, &keys[1]) == &visited[1]);
    for (size_t i = 0; i < FIRST_BATCH; i += 2)
        hashmap_delete_pointer(&map, &keys[i]);
    for (size_t i = FIRST_BATCH; i < KEY_COUNT; ++i)
        hashmap_put_pointer(&map, &keys[i], &visited[i]);
    hashmap_put_pointer(&map, NULL, &visited_null);
    assert(hashmap_get_pointer(&map, NULL) == &visited_null);

    hashmap_foreach_value(&map, mark_visited);
    assert(visited_null);
    for (size_t i = 0; i < KEY_COUNT; ++i) {
        bool live = i >= FIRST_BATCH || i % 2 != 0;
        assert(hashmap_get_pointer(&map, &keys[i]) == (live ? &visited[i] : NULL));
        assert(visited[i] == live);
    }
    hashmap_delete_pointer(&map, NULL);
    assert(hashmap_get_pointer(&map, NULL) == NULL);

    hashmap_free(&map);
    assert(hashmap_get_pointer(&map, &keys[1]) == NULL);
    hashmap_put_pointer(&map, &keys[1], &visited[1]);
    assert(hashmap_get_pointer(&map, &keys[1]) == &visited[1]);
    hashmap_free(&map);
}

// Force two pointer keys into one probe chain and update behind a tombstone.
static void test_pointer_update_behind_tombstone(void)
{
    int keys[INITIAL_CAPACITY + 1] = {0};
    const int *seen[INITIAL_CAPACITY] = {0};
    const int *first = NULL;
    const int *second = NULL;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        HashKey key = { .pointer = &keys[i] };
        size_t slot = hash_key(HASHMAP_KEY_POINTER, key) % INITIAL_CAPACITY;
        if (seen[slot]) {
            first = seen[slot];
            second = &keys[i];
            break;
        }
        seen[slot] = &keys[i];
    }
    assert(first && second);

    int initial = 1;
    int replacement = 2;
    HashMap map;
    hashmap_init(&map, HASHMAP_KEY_POINTER);
    hashmap_put_pointer(&map, first, &initial);
    hashmap_put_pointer(&map, second, &initial);
    hashmap_delete_pointer(&map, first);
    hashmap_put_pointer(&map, second, &replacement);
    assert(hashmap_get_pointer(&map, second) == &replacement);
    hashmap_delete_pointer(&map, second);
    assert(hashmap_get_pointer(&map, second) == NULL);
    hashmap_free(&map);
}

// Run both key families against the same table implementation.
// Verify full-width numeric identity, growth, replacement and deletion.
static void test_uint64_keys(void)
{
    HashMap map;
    hashmap_init(&map, HASHMAP_KEY_UINT64);
    int values[64];
    const size_t count = sizeof(values) / sizeof(values[0]);
    for (size_t i = 0; i < count; ++i)
        hashmap_put_uint64(&map, (uint64_t)i << 48, &values[i]);
    hashmap_put_uint64(&map, UINT64_MAX, &values[1]);
    for (size_t i = 0; i < count; ++i)
        assert(hashmap_get_uint64(&map, (uint64_t)i << 48) == &values[i]);
    assert(hashmap_get_uint64(&map, UINT64_MAX) == &values[1]);
    hashmap_delete_uint64(&map, 0);
    assert(hashmap_get_uint64(&map, 0) == NULL);
    hashmap_put_uint64(&map, UINT64_MAX, &values[2]);
    assert(hashmap_get_uint64(&map, UINT64_MAX) == &values[2]);
    hashmap_free(&map);
}

int main(void)
{
    test_byte_keys();
    test_pointer_keys();
    test_uint64_keys();
    test_pointer_update_behind_tombstone();
    puts("OK");
    return 0;
}

#endif
