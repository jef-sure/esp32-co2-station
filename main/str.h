/*
 * str.h
 *
 *  Created on: Mar 16, 2021
 *      Author: anton
 */

#ifndef _STR_H_
#define _STR_H_

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef STR_LOG_TAG
#define STR_LOG_TAG "str"
#endif

typedef struct _str_t
{
    size_t capacity;
    size_t length;
    char data[1];
} str_t;

typedef struct _str_vector_t
{
    size_t length;
    str_t **vector;
} str_vector_t;

static const size_t str_npos = (size_t)(-1);
extern str_t str_empty;

str_t *str_new_ln(size_t ln);
str_t *str_new_pc(const char *pc);
str_t *str_new_pcln(const char *pc, size_t ln);
str_t *str_new_str(const str_t *str);
void str_destroy(str_t **dst);
static inline str_t *str_clear(str_t *str)
{
    if (str == NULL || str == &str_empty)
        return str;
    str->length = 0;
    return str;
}

static inline char *str_c(str_t *str)
{
    if (str == NULL)
        return NULL;
    if (str == &str_empty)
        return str->data;
    str->data[str->length] = 0;
    return str->data;
}

static inline char *str_data(str_t *str)
{
    return str_c(str);
}

static inline size_t str_bufsize(const str_t *str)
{
    return str != NULL ? str->capacity : 0;
}

static inline size_t str_length(const str_t *str)
{
    return str != NULL ? str->length : 0;
}

bool str_is_empty(const str_t *str);
bool str_endswith_pc(const str_t *str, const char *pc);
bool str_startswith_pc(const str_t *str, const char *pc);
int str_cmp_str(const str_t *str, const str_t *str2);

str_t *str_substr(const str_t *str, size_t pos, size_t len);
void str_alltrim(str_t **dst_str);
str_t *str_chomp(str_t *str);
str_t *str_tolower(str_t *str);
str_t *str_toupper(str_t *str);

str_t *str_append_str(str_t **dest_str, const str_t *str);
str_t *str_append_pc(str_t **dest_str, const char *pc);
str_t *str_append_pcln(str_t **dest_str, const char *pc, size_t sln);
str_t *str_append_c(str_t **dest_str, char c);
str_t *str_replace_str(str_t **dest_str, size_t pos, size_t n, const str_t *str);
size_t str_find_str(const str_t *str, const str_t *substr, size_t pos);
size_t str_find_pc(const str_t *str, const char *pc, size_t pos);
size_t str_find_c(const str_t *str, char c, size_t pos);
size_t str_rfind_c(const str_t *str, char c, size_t pos);
size_t str_rfind_str(const str_t *str, const str_t *substr, size_t pos);
size_t str_rfind_pc(const str_t *str, const char *pc, size_t pos);

str_vector_t *str_vector_new(size_t ln);
bool str_vector_add_str(str_vector_t *strv, str_t *str);
void str_vector_shrink(str_vector_t *strv);
void str_vector_destroy(str_vector_t **pstrv);
str_vector_t *str_split_pc(const str_t *str, const char *pc);

#endif /* _STR_H_ */