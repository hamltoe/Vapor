/* Small read-side helpers over cJSON. Every API response is parsed in a few
 * places, and each one needs the same "give me this field or a default"
 * behaviour, so it is written once here.
 *
 * Internal to the client core; not part of the installed headers. */

#ifndef VAPOR_CLIENT_JSON_H
#define VAPOR_CLIENT_JSON_H

#include <stddef.h>

#include "cJSON.h"
#include "vapor/client.h"

/* NULL if the response had no body or it was not valid JSON. */
cJSON *vapor_json_parse_response(const vapor_response *r);

const char *vapor_json_str(const cJSON *obj, const char *key,
                           const char *fallback);
double      vapor_json_num(const cJSON *obj, const char *key, double fallback);
int         vapor_json_bool(const cJSON *obj, const char *key, int fallback);

/* Copies a string field into a fixed buffer, truncating rather than failing:
 * a name too long for the field is a display problem, not an error. */
void vapor_json_copy(char *out, size_t outsz, const cJSON *obj, const char *key,
                     const char *fallback);

#endif /* VAPOR_CLIENT_JSON_H */
