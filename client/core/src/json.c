#include "json.h"

#include <stdio.h>

cJSON *
vapor_json_parse_response(const vapor_response *r)
{
    if (!r || !r->body || r->body_len == 0) {
        return NULL;
    }
    return cJSON_ParseWithLength(r->body, r->body_len);
}

const char *
vapor_json_str(const cJSON *obj, const char *key, const char *fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsString(it) && it->valuestring) {
        return it->valuestring;
    }
    return fallback;
}

double
vapor_json_num(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsNumber(it) ? it->valuedouble : fallback;
}

int
vapor_json_bool(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it) ? 1 : 0;
    }
    return fallback;
}

void
vapor_json_copy(char *out, size_t outsz, const cJSON *obj, const char *key,
                const char *fallback)
{
    const char *s = vapor_json_str(obj, key, fallback);

    if (outsz == 0) {
        return;
    }
    snprintf(out, outsz, "%s", s ? s : "");
}
