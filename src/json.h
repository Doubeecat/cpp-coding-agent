#ifndef CA_JSON_H
#define CA_JSON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson/yyjson.h>

typedef yyjson_mut_val JVal;
typedef struct Json {
    yyjson_mut_doc *doc;
    JVal *root;
    bool failed;
} Json;
#define JV(j) ((j) ? (j)->root : NULL)
Json *json_object(void);
Json *json_array(void);
Json *json_parse(const char *bytes, size_t len);
Json *json_literal(const char *text);
Json *json_clone(const JVal *value);
Json *json_event(const char *type);
void json_free(Json *json);
JVal *json_get(const JVal *object, const char *key);
JVal *json_at(const JVal *array, size_t index);
const char *json_str(const JVal *value);
const char *json_string(const JVal *object, const char *key, const char *fallback);
size_t json_len(const JVal *value);
bool json_is_object(const JVal *value);
bool json_is_array(const JVal *value);
bool json_is_string(const JVal *value);
bool json_is_int(const JVal *value);
bool json_is_number(const JVal *value);
bool json_is_bool(const JVal *value);
bool json_is_null(const JVal *value);
int64_t json_integer(const JVal *value, int64_t fallback);
int64_t json_int(const JVal *object, const char *key, int64_t fallback);
double json_number(const JVal *object, const char *key, double fallback);
bool json_bool(const JVal *object, const char *key, bool fallback);
bool json_equal(const JVal *left, const JVal *right);
JVal *json_new_object(Json *json);
JVal *json_new_array(Json *json);
int json_add(Json *json, JVal *object, const char *key, JVal *value);
int json_add_string(Json *json, JVal *object, const char *key, const char *text, size_t len);
int json_set_string(Json *json, const char *key, const char *text);
int json_set_stringn(Json *json, const char *key, const char *text, size_t len);
int json_set_int(Json *json, const char *key, int64_t value);
int json_set_bool(Json *json, const char *key, bool value);
int json_set_null(Json *json, const char *key);
int json_set_value(Json *json, const char *key, const JVal *value);
int json_append(Json *json, const JVal *value);
int json_append_string(Json *json, const char *text);
int json_remove_first(Json *json);
char *json_dump_value(const JVal *value, bool pretty, size_t *len);
char *json_dump(const Json *json, bool pretty, size_t *len);
#endif
