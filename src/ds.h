#ifndef DS_H
#define DS_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "util.h"

#define DA_INITIAL_CAPACITY 64

#define DA_DEFINE(type, name)                                                                                   \
	struct name                                                                                             \
	{                                                                                                       \
		size_t capacity;                                                                                \
		size_t len;                                                                                     \
		type elems[];                                                                                   \
	};                                                                                                      \
	NO_DISCARD static inline struct name *name##_create(size_t capacity)                                    \
	{                                                                                                       \
		assert(capacity != 0);                                                                          \
		struct name *alloc = (struct name *)calloc(1, sizeof(struct name) + capacity * sizeof(type));   \
		if (!alloc)                                                                                     \
			fputs("Allocation failure: buy more ram lol", stderr);                                  \
		else                                                                                            \
			alloc->capacity = capacity;                                                             \
		return alloc;                                                                                   \
	}                                                                                                       \
	NO_DISCARD static inline struct name *name##_append(struct name *da, type elem)                         \
	{                                                                                                       \
		if (da->len >= da->capacity)                                                                    \
		{                                                                                               \
			struct name *temp = realloc(da, sizeof(struct name) + da->capacity * 2 * sizeof(type)); \
			if (!temp)                                                                              \
			{                                                                                       \
				fputs("Allocation failure: buy more ram lol", stderr);                          \
				return da;                                                                      \
			}                                                                                       \
			da = temp;                                                                              \
			da->capacity *= 2;                                                                      \
		}                                                                                               \
		da->elems[da->len++] = elem;                                                                    \
		return da;                                                                                      \
	}                                                                                                       \

#endif