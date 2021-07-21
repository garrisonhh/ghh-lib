#define GHH_LIB_INTERNAL
#include <stdlib.h>
#include <stdint.h>
#include <string.h> // only for memcpy, don't use libc str functions when avoidable
#include <ghh/hashmap.h>
#include <ghh/utils.h>

#if INTPTR_MAX == INT64_MAX

// 64 bit
typedef uint64_t hash_t;
#define FNV_PRIME 0x00000100000001b3
#define FNV_BASIS 0xcbf29ce484222325

#else

// 32 bit
typedef uint32_t hash_t;
#define FNV_PRIME 0x01000193a
#define FNV_BASIS 0x811c9dc5

#endif

#define MIN_HASHMAP_SIZE 8

typedef struct hashbucket {
	bool filled;
	int desired_idx;
	hash_t hash;
	void *key, *value;
} hashbucket_t;

struct ghh_hashmap {
	hashbucket_t *buckets;
	size_t size, min_size, alloc_size;

	int key_size;
	bool copy_keys;
};

struct ghh_hmapiter {
	hashmap_t *hmap;
	int idx;
};

hashmap_t *hashmap_create(size_t initial_size, int key_size, bool copy_keys) {
	hashmap_t *hmap = malloc(sizeof(*hmap));

	hmap->key_size = key_size;
	hmap->copy_keys = copy_keys;

	hmap->size = 0;
	hmap->min_size = MAX(initial_size, MIN_HASHMAP_SIZE);
	hmap->alloc_size = hmap->min_size;
	hmap->buckets = calloc(hmap->alloc_size, sizeof(*hmap->buckets));

	return hmap;
}

void hashmap_destroy(hashmap_t *hmap, bool destroy_values) {
	if (destroy_values)
		for (size_t i = 0; i < hmap->alloc_size; ++i)
			if (hmap->buckets[i].filled)
				free(hmap->buckets[i].value);

	if (hmap->copy_keys)
		for (size_t i = 0; i < hmap->alloc_size; ++i)
			if (hmap->buckets[i].key != NULL)
				free(hmap->buckets[i].key);

	free(hmap->buckets);
	free(hmap);
}

size_t hashmap_size(hashmap_t *hmap) {
	return hmap->size;
}

// fnv-1a hash function (http://isthe.com/chongo/tech/comp/fnv/)
static inline hash_t hash_key(hashmap_t *hmap, const void *key) {
	hash_t hash = FNV_BASIS;
	const uint8_t *bytes = key;

	if (hmap->key_size < 0) {
		while (*bytes) {
			hash ^= *bytes++;
			hash *= FNV_PRIME;
		}
	} else {
		for (int i = 0; i < hmap->key_size; ++i) {
			hash ^= bytes[i];
			hash *= FNV_PRIME;
		}
	}

	return hash;
}

static inline const void *copy_key(hashmap_t *hmap, const void *key) {
	void *copied;

	if (hmap->key_size < 0) {
		size_t num_bytes = 1;
		char *str = (char *)key;

		while (*str++)
			++num_bytes;

		num_bytes *= sizeof(*str);

		copied = malloc(num_bytes);
		memcpy(copied, key, num_bytes);
	} else {
		copied = malloc(hmap->key_size);
		memcpy(copied, key, hmap->key_size);
	}

	return copied;
}

static inline bool key_equals(hashmap_t *hmap, const void *key, const void *other) {
	size_t i = 0;
	const uint8_t *kbytes = key, *obytes = other;

	if (hmap->key_size < 0) {
		do {
			if (kbytes[i] != obytes[i])
				return false;
		} while (kbytes[i++]);
	} else {
		for (; i < hmap->key_size; ++i)
			if (kbytes[i] != obytes[i])
				return false;
	}

	return true;
}

// returns -1 if no bucket found
static inline int get_bucket_index(hashmap_t *hmap, const void *key, hash_t hash) {
	int index = hash % hmap->alloc_size;

	while (hmap->buckets[index].filled) {
		if (hmap->buckets[index].hash == hash && key_equals(hmap, key, hmap->buckets[index].key))
			break;

		index = (index + 1) % hmap->alloc_size;
	}

	return index;
}

// returns old value, null if none found
static inline void *hashmap_set_lower(hashmap_t *hmap, void *key, void *value, hash_t hash) {
	int index;
	void *old_value = NULL;

	index = get_bucket_index(hmap, key, hash);

	if (hmap->buckets[index].filled) {
		if (hmap->copy_keys)
			free(hmap->buckets[index].key);

		old_value = hmap->buckets[index].value;
	} else {
		hmap->buckets[index].filled = true;
		hmap->buckets[index].hash = hash;

		++hmap->size;
	}

	hmap->buckets[index].key = key;
	hmap->buckets[index].value = value;

	// this should only be needed for rehash, TODO some fix?
	hmap->buckets[index].desired_idx = hmap->buckets[index].hash % hmap->alloc_size;

	return old_value;
}

static inline void rehash(hashmap_t *hmap, size_t new_size) {
	if (new_size < hmap->min_size)
		return;

	hashbucket_t *old_buckets;
	size_t old_size;

	old_buckets = hmap->buckets;
	old_size = hmap->alloc_size;

	hmap->size = 0;
	hmap->alloc_size = new_size;
	hmap->buckets = calloc(hmap->alloc_size, sizeof(*hmap->buckets));

	for (size_t i = 0; i < old_size; ++i) {
		if (old_buckets[i].filled) {
			hashmap_set_lower(
				hmap,
				old_buckets[i].key,
				old_buckets[i].value,
				old_buckets[i].hash
			);
		}
	}

	free(old_buckets);
}

void *hashmap_get(hashmap_t *hmap, const void *key) {
	int index = get_bucket_index(hmap, key, hash_key(hmap, key));

	return hmap->buckets[index].filled ? hmap->buckets[index].value : NULL;
}

bool hashmap_may_get(hashmap_t *hmap, const void *key, void **out_value) {
	int index = get_bucket_index(hmap, key, hash_key(hmap, key));

	if (hmap->buckets[index].filled) {
		if (out_value != NULL)
			*out_value = hmap->buckets[index].value;

		return true;
	}

	return false;
}

bool hashmap_has(hashmap_t *hmap, const void *key) {
	return hmap->buckets[get_bucket_index(hmap, key, hash_key(hmap, key))].filled;
}

void *hashmap_set(hashmap_t *hmap, const void *key, const void *value) {
	if (hmap->size >= (hmap->alloc_size >> 1))
		rehash(hmap, hmap->alloc_size << 1);

	if (hmap->copy_keys)
		key = copy_key(hmap, key);

	return hashmap_set_lower(hmap, (void *)key, (void *)value, hash_key(hmap, key));
}

void *hashmap_remove(hashmap_t *hmap, const void *key) {
	hash_t hash;
	int index;
	void *value = NULL;

	index = get_bucket_index(hmap, key, (hash = hash_key(hmap, key)));

	if (hmap->buckets[index].filled) {
		// filter up slots connected to this one
		int last_index = index, desired;
		bool filter;

		value = hmap->buckets[index].value;

		if (hmap->copy_keys)
			free(hmap->buckets[index].key);

		while (1) {
			// increment with wrap
			index = (index + 1) % hmap->alloc_size;

			// stop if an empty slot is reached
			if (!hmap->buckets[index].filled)
				break;

			// flag whether to filter up
			filter = false;
			desired = hmap->buckets[index].desired_idx;

			if (desired < index) { // no bucket wrap
				if (last_index < index && desired <= last_index)
					filter = true;
			} else if (desired > index) { // bucket wrap
				if (last_index < index || (/*last_index > index &&*/ desired <= last_index))
					filter = true;
			}

			// filter up
			if (filter) {
				/*printf(
					"filtering up %d <- %d (wants %d)\n",
					last_index, index, hmap->buckets[index].desired_idx
				);*/
				hmap->buckets[last_index] = hmap->buckets[index];
				last_index = index;
			}
		}

		hmap->buckets[last_index].filled = false;

		if (--hmap->size < (hmap->alloc_size >> 2))
			rehash(hmap, hmap->alloc_size >> 1);
	}

	return value;
}

hmapiter_t *hmapiter_create(hashmap_t *hmap) {
	hmapiter_t *iter = malloc(sizeof(*iter));

	iter->hmap = hmap;
	iter->idx = -1;

	return iter;
}

void hmapiter_reset(hmapiter_t *iter) {
	iter->idx = -1;
}

// return whether iteration is not done
bool hmapiter_next(hmapiter_t *iter, void **out_key, void **out_value) {
	do {
		if (++iter->idx >= iter->hmap->alloc_size) {
			iter->idx = -1;

			if (out_key != NULL)
				*out_key = NULL;
			if (out_value != NULL)
				*out_value = NULL;

			return false;
		}
	} while (!iter->hmap->buckets[iter->idx].filled);

	if (out_key != NULL)
		*out_key = iter->hmap->buckets[iter->idx].key;
	if (out_value != NULL)
		*out_value = iter->hmap->buckets[iter->idx].value;

	return true;
}
