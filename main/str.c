#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#include "str.h"

#ifndef STR_LOGE
#ifdef ESP_PLATFORM
#define STR_LOGE ESP_LOGE
#else
#define STR_LOGE(tag, fmt, ...) ((void)0)
#endif
#endif

static const char *TAG = STR_LOG_TAG;

static size_t _str_allign(size_t ln)
{
	ln += 4 - ((ln + sizeof(str_t)) & 3);
	return ln;
}

str_t str_empty = {1u, 0u, {0}};

str_t *str_new_ln(size_t ln)
{
	if (ln == 0)
		return &str_empty;
	str_t *ret;
	size_t cap = _str_allign(ln);
	ret = (str_t *)malloc(sizeof(str_t) + cap);
	if (ret == NULL)
	{
		STR_LOGE(TAG, "memory pressure: failed to allocate string length %u", (unsigned int)ln);
		return NULL;
	}
	ret->length = ln;
	ret->capacity = cap;
	return ret;
}

str_t *str_new_pc(const char *pc)
{
	size_t sln = strlen(pc);
	if (sln == 0)
		return &str_empty;
	str_t *ret = str_new_ln(sln);
	if (ret == NULL)
		return NULL;
	memcpy(ret->data, pc, sln);
	return ret;
}

str_t *str_new_pcln(const char *pc, size_t ln)
{
	if (ln == 0)
		return &str_empty;
	str_t *ret = str_new_ln(ln);
	if (ret == NULL)
		return NULL;
	memcpy(ret->data, pc, ln);
	return ret;
}

str_t *str_new_str(const str_t *str)
{
	return str_new_pcln(str->data, str->length);
}

void str_destroy(str_t **dst)
{
	if (dst == NULL || *dst == NULL)
		return;
	if (*dst != &str_empty)
		free(*dst);
	*dst = NULL;
}

bool str_is_empty(const str_t *str)
{
	size_t r;
	if (str_length(str) == 0)
	{
		return 1;
	}
	for (r = 0; r < str_length(str) && isspace((unsigned char)str->data[r]); ++r)
		;
	if (r == str->length)
	{
		return 1;
	}
	return 0;
}

bool str_endswith_pc(const str_t *str, const char *pc)
{
	size_t sln = strlen(pc);
	if (str->length < sln)
	{
		return 0;
	}
	return memcmp(str->data + (str->length - sln), pc, sln) == 0;
}

bool str_startswith_pc(const str_t *str, const char *pc)
{
	size_t sln = strlen(pc);
	if (str->length < sln)
	{
		return 0;
	}
	return memcmp(str->data, pc, sln) == 0;
}

int str_cmp_str(const str_t *str, const str_t *str2)
{
	size_t minlen = str->length > str2->length ? str2->length : str->length;
	if (minlen == 0)
	{
		if (str->length == 0 && str2->length == 0)
			return 0;
		else if (str->length == 0)
			return -1;
		else
			return 1;
	}
	int r = memcmp(str->data, str2->data, minlen);
	if (r != 0)
		return r;
	if (str->length == str2->length)
		return 0;
	else if (str->length < str2->length)
		return -1;
	else
		return 1;
}

str_t *str_substr(const str_t *str, size_t pos, size_t len)
{
	if (pos >= str->length || len == 0)
		return &str_empty;
	if (len == str_npos || pos + len > str->length)
		len = str->length - pos;
	return str_new_pcln(str->data + pos, len);
}

void str_alltrim(str_t **dst_str)
{
	if (dst_str == NULL || *dst_str == NULL || *dst_str == &str_empty)
		return;
	size_t p1, p2;
	str_t *tmp;
	for (p1 = 0; p1 < (*dst_str)->length && isspace((unsigned char)(*dst_str)->data[p1]); ++p1)
		;
	if (p1 == (*dst_str)->length)
	{
		(*dst_str)->length = 0;
		return;
	}
	for (p2 = (*dst_str)->length - 1; p2 + 1 > p1 && isspace((unsigned char)(*dst_str)->data[p2]); --p2)
		;
	tmp = str_substr(*dst_str, p1, p2 - p1 + 1);
	if (tmp == NULL)
		return;
	str_destroy(dst_str);
	*dst_str = tmp;
}

str_t *str_chomp(str_t *str)
{
	char c;
	if (str != NULL)
		while (str->length != 0 && ((c = str->data[str->length - 1]) == '\r' || c == '\n'))
			--str->length;
	return str;
}

str_t *str_tolower(str_t *str)
{
	size_t i;
	for (i = 0; i < str->length; ++i)
		str->data[i] = (char)tolower((unsigned char)str->data[i]);
	return str;
}

str_t *str_toupper(str_t *str)
{
	size_t i;
	for (i = 0; i < str->length; ++i)
		str->data[i] = (char)toupper((unsigned char)str->data[i]);
	return str;
}

str_t *str_append_str(str_t **dest_str, const str_t *str)
{
	if (str == NULL || str->length == 0)
		return *dest_str;
	if (*dest_str == NULL || *dest_str == &str_empty)
	{
		str_t *tmp = str_new_str(str);
		if (tmp != NULL)
			*dest_str = tmp;
		else
			STR_LOGE(TAG, "str_append_str: failed to allocate memory for string of length %u", (unsigned int)str->length);
	}
	else
	{
		if ((*dest_str)->capacity < (*dest_str)->length + str->length + 1)
		{
			str_t *tmp = str_new_ln((*dest_str)->capacity + (str->capacity * 2));
			if (tmp == NULL)
			{
				STR_LOGE(TAG, "str_append_str: failed to reallocate memory for combined length %u", (unsigned int)((*dest_str)->length + str->length));
				return *dest_str;
			}
			memcpy(tmp->data, (*dest_str)->data, (*dest_str)->length);
			memcpy(tmp->data + (*dest_str)->length, str->data, str->length);
			tmp->length = (*dest_str)->length + str->length;
			free(*dest_str);
			*dest_str = tmp;
		}
		else
		{
			memcpy((*dest_str)->data + (*dest_str)->length, str->data, str->length);
			(*dest_str)->length += str->length;
		}
	}
	return *dest_str;
}

str_t *str_append_pc(str_t **dest_str, const char *pc)
{
	size_t sln;
	if (pc == NULL || (sln = strlen(pc)) == 0)
		return *dest_str;
	if (*dest_str == NULL || *dest_str == &str_empty)
	{
		str_t *tmp = str_new_pc(pc);
		if (tmp != NULL)
			*dest_str = tmp;
		else
			STR_LOGE(TAG, "str_append_pc: failed to allocate memory for string of length %u", (unsigned int)sln);
	}
	else
	{
		if ((*dest_str)->capacity < (*dest_str)->length + sln + 1)
		{
			str_t *tmp = str_new_ln((*dest_str)->capacity + (sln * 2));
			if (tmp == NULL)
			{
				STR_LOGE(TAG, "str_append_pc: failed to reallocate memory for combined length %u", (unsigned int)((*dest_str)->length + sln));
				return *dest_str;
			}
			memcpy(tmp->data, (*dest_str)->data, (*dest_str)->length);
			memcpy(tmp->data + (*dest_str)->length, pc, sln);
			tmp->length = (*dest_str)->length + sln;
			free(*dest_str);
			*dest_str = tmp;
		}
		else
		{
			memcpy((*dest_str)->data + (*dest_str)->length, pc, sln);
			(*dest_str)->length += sln;
		}
	}
	return *dest_str;
}

str_t *str_append_pcln(str_t **dest_str, const char *pc, size_t sln)
{
	if (pc == NULL || sln == 0)
		return *dest_str;
	if (*dest_str == NULL || *dest_str == &str_empty)
	{
		str_t *tmp = str_new_pcln(pc, sln);
		if (tmp != NULL)
			*dest_str = tmp;
		else
			STR_LOGE(TAG, "str_append_pcln: failed to allocate memory for string of length %u", (unsigned int)sln);
	}
	else
	{
		if ((*dest_str)->capacity < (*dest_str)->length + sln + 1)
		{
			str_t *tmp = str_new_ln((*dest_str)->capacity + (sln * 2));
			if (tmp == NULL)
			{
				STR_LOGE(TAG, "str_append_pcln: failed to reallocate memory for combined length %u", (unsigned int)((*dest_str)->length + sln));
				return *dest_str;
			}
			memcpy(tmp->data, (*dest_str)->data, (*dest_str)->length);
			memcpy(tmp->data + (*dest_str)->length, pc, sln);
			tmp->length = (*dest_str)->length + sln;
			free(*dest_str);
			*dest_str = tmp;
		}
		else
		{
			memcpy((*dest_str)->data + (*dest_str)->length, pc, sln);
			(*dest_str)->length += sln;
		}
	}
	return *dest_str;
}

str_t *str_append_c(str_t **dest_str, char c)
{
	if (*dest_str == NULL || *dest_str == &str_empty)
	{
		str_t *tmp = str_new_pcln(&c, 1);
		if (tmp != NULL)
			*dest_str = tmp;
		else
			STR_LOGE(TAG, "str_append_c: failed to allocate memory for string of length 1");
	}
	else
	{
		if ((*dest_str)->capacity < (*dest_str)->length + 2)
		{
			str_t *tmp = str_new_ln((*dest_str)->capacity + 16);
			if (tmp == NULL)
			{
				STR_LOGE(TAG, "str_append_c: failed to reallocate memory for combined length %u", (unsigned int)((*dest_str)->length + 1));
				return *dest_str;
			}
			memcpy(tmp->data, (*dest_str)->data, (*dest_str)->length);
			tmp->data[(*dest_str)->length] = c;
			tmp->length = (*dest_str)->length + 1;
			free(*dest_str);
			*dest_str = tmp;
		}
		else
		{
			(*dest_str)->data[(*dest_str)->length] = c;
			(*dest_str)->length += 1;
		}
	}
	return *dest_str;
}

str_t *str_replace_str(str_t **dest_str, size_t pos, size_t n, const str_t *str)
{
	size_t nl;
	str_t *tmp;
	if (pos == (*dest_str)->length)
	{
		str_append_str(dest_str, str);
		return *dest_str;
	}
	if (pos > (*dest_str)->length)
		return *dest_str;
	if (str == NULL)
		str = &str_empty;
	if (n == str_npos || pos + n > (*dest_str)->length)
		n = (*dest_str)->length - pos;
	nl = (*dest_str)->length + str->length - n;
	tmp = str_new_ln(nl);
	if (tmp == NULL)
		return *dest_str;
	if (pos)
		memcpy(tmp->data, (*dest_str)->data, pos);
	if (str->length)
		memcpy(tmp->data + pos, str->data, str->length);
	if (pos + n < (*dest_str)->length)
		memcpy(tmp->data + pos + str->length, (*dest_str)->data + pos + n, (*dest_str)->length - n - pos);
	str_destroy(dest_str);
	*dest_str = tmp;
	return *dest_str;
}

size_t str_find_str(const str_t *str, const str_t *substr, size_t pos)
{
	if (str == NULL)
		return str_npos;
	if (substr->length > str->length || str->length == 0 || substr->length == 0)
	{
		return str_npos;
	}
	while (pos + substr->length <= str->length)
		if (memcmp(str->data + pos, substr->data, substr->length) == 0)
			return pos;
		else
			++pos;
	return str_npos;
}

size_t str_find_pc(const str_t *str, const char *pc, size_t pos)
{
	if (str == NULL || pc == NULL)
		return str_npos;
	size_t sln = strlen(pc);
	if (sln > str->length || str->length == 0 || sln == 0)
	{
		return str_npos;
	}
	while (pos + sln <= str->length)
		if (memcmp(str->data + pos, pc, sln) == 0)
			return pos;
		else
			++pos;
	return str_npos;
}

size_t str_find_c(const str_t *str, char c, size_t pos)
{
	char *cptr;
	if (str == NULL)
		return str_npos;
	if (pos >= str->length || str->length == 0)
	{
		return str_npos;
	}
	cptr = (char *)memchr(str->data + pos, c, str->length - pos);
	if (cptr != NULL)
	{
		return cptr - str->data;
	}
	return str_npos;
}

size_t str_rfind_c(const str_t *str, char c, size_t pos)
{
	if (str == NULL)
		return str_npos;
	if (str->length == 0)
	{
		return str_npos;
	}
	if (pos >= str->length)
		pos = str->length - 1;
	while (pos + 1 > 0)
	{
		if (str->data[pos] == c)
		{
			return pos;
		}
		--pos;
	}
	return str_npos;
}

size_t str_rfind_str(const str_t *str, const str_t *substr, size_t pos)
{
	if (str == NULL)
		return str_npos;
	if (substr->length > str->length || str->length == 0 || substr->length == 0)
	{
		return str_npos;
	}
	if (pos == str_npos || pos + substr->length > str->length)
		pos = str->length - substr->length;
	while (pos + 1 > 0)
		if (memcmp(str->data + pos, substr->data, substr->length) == 0)
		{
			return pos;
		}
		else
			--pos;
	return str_npos;
}

size_t str_rfind_pc(const str_t *str, const char *pc, size_t pos)
{
	if (str == NULL || pc == NULL)
		return str_npos;
	size_t sln = strlen(pc);
	if (sln > str->length || str->length == 0 || sln == 0)
	{
		return str_npos;
	}
	if (pos == str_npos || pos + sln > str->length)
		pos = str->length - sln;
	while (pos + 1 > 0)
		if (memcmp(str->data + pos, pc, sln) == 0)
			return pos;
		else
			--pos;
	return str_npos;
}

str_vector_t *str_vector_new(size_t ln)
{
	str_vector_t *ret = (str_vector_t *)malloc(sizeof(*ret));
	if (ret == NULL)
	{
		STR_LOGE(TAG, "memory pressure: failed to allocate string vector");
		return NULL;
	}
	ret->vector = (str_t **)calloc(ln, sizeof(str_t *));
	if (ret->vector == NULL && ln != 0)
	{
		STR_LOGE(TAG, "memory pressure: failed to allocate string vector with %u slots", (unsigned int)ln);
		free(ret);
		return NULL;
	}
	ret->length = ln;
	return ret;
}

bool str_vector_add_str(str_vector_t *strv, str_t *str)
{
	if (str == NULL)
	{
		STR_LOGE(TAG, "str_vector_add_str: str is NULL - nothing is added");
		return false;
	}
	if (strv == NULL)
	{
		STR_LOGE(TAG, "str_vector_add_str: vector is NULL, string '%s' is destroyed", str_c(str));
		str_destroy(&str);
		return false;
	}
	str_t **new_vector = (str_t **)realloc(strv->vector, (strv->length + 1) * sizeof(str_t *));
	if (new_vector == NULL)
	{
		STR_LOGE(TAG, "str_vector_add_str: realloc failed, string '%s' is destroyed", str_c(str));
		str_destroy(&str);
		return false;
	}
	strv->vector = new_vector;
	strv->vector[strv->length] = str;
	++strv->length;
	return true;
}

void str_vector_shrink(str_vector_t *strv)
{
	if (strv == NULL || strv->length == 0)
		return;
	str_destroy(strv->vector + strv->length - 1);
	--strv->length;
}

void str_vector_destroy(str_vector_t **pstrv)
{
	if (pstrv != NULL && *pstrv != NULL)
	{
		size_t n;
		for (n = 0; n < (*pstrv)->length; ++n)
			str_destroy((*pstrv)->vector + n);
		free((*pstrv)->vector);
		free(*pstrv);
		*pstrv = NULL;
	}
}

str_vector_t *str_split_pc(const str_t *str, const char *pc)
{
	size_t sln;
	str_vector_t *ret;
	str_t *part;
	if (str_length(str) == 0)
	{
		ret = str_vector_new(0);
		if (ret == NULL)
			return NULL;
		part = str_new_ln(0);
		if (part == NULL)
		{
			STR_LOGE(TAG, "str_split_pc: failed to allocate empty split part");
			str_vector_destroy(&ret);
			return NULL;
		}
		if (!str_vector_add_str(ret, part))
		{
			STR_LOGE(TAG, "str_split_pc: failed to append empty split part");
			str_vector_destroy(&ret);
			return NULL;
		}
		return ret;
	}
	if (pc == NULL || (sln = strlen(pc)) == 0)
	{
		size_t i;
		ret = str_vector_new(str->length);
		if (ret == NULL)
			return NULL;
		for (i = 0; i < str->length; ++i)
		{
			ret->vector[i] = str_new_pcln(str->data + i, 1);
			if (ret->vector[i] == NULL)
			{
				STR_LOGE(TAG, "str_split_pc: failed to allocate character split part at position %u", (unsigned int)i);
				str_vector_destroy(&ret);
				return NULL;
			}
		}
		return ret;
	}

	ret = str_vector_new(0);
	if (ret == NULL)
		return NULL;
	size_t npos = 0, fpos;
	do
	{
		fpos = str_find_pc(str, pc, npos);
		if (fpos != str_npos)
			part = str_substr(str, npos, fpos - npos);
		else
			part = str_substr(str, npos, str->length - npos);

		if (part == NULL)
		{
			STR_LOGE(TAG, "str_split_pc: failed to allocate split part at position %u", (unsigned int)npos);
			str_vector_destroy(&ret);
			return NULL;
		}
		if (!str_vector_add_str(ret, part))
		{
			STR_LOGE(TAG, "str_split_pc: failed to append split part at position %u", (unsigned int)npos);
			str_vector_destroy(&ret);
			return NULL;
		}
		if (fpos == str_npos)
			break;
		npos = fpos + sln;
	} while (1);
	return ret;
}
