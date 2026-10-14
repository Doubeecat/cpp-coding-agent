#include "json.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static Json *new_doc(void) {
    Json *json = calloc(1, sizeof(*json));
    if (!json)
        return NULL;
    json->doc = yyjson_mut_doc_new(NULL);
    if (!json->doc) {
        free(json);
        return NULL;
    }
    return json;
}
Json *json_object(void) {
    Json *json = new_doc();
    if (json) {
        json->root = yyjson_mut_obj(json->doc);
        json->failed = !json->root;
    }
    return json;
}
Json *json_array(void) {
    Json *json = new_doc();
    if (json) {
        json->root = yyjson_mut_arr(json->doc);
        json->failed = !json->root;
    }
    return json;
}
static JVal *copy_read(Json *json, const yyjson_val *value, unsigned depth) {
    if (depth > 128)
        return NULL;
    if (yyjson_is_obj(value)) {
        JVal *object = yyjson_mut_obj(json->doc);
        yyjson_val *key, *child;
        size_t index, count;
        yyjson_obj_foreach((yyjson_val *)value, index, count, key, child) {
            JVal *k = yyjson_val_mut_copy(json->doc, key);
            JVal *v = copy_read(json, child, depth + 1);
            if (!k || !v || !yyjson_mut_obj_put(object, k, v))
                return NULL;
        }
        return object;
    }
    if (yyjson_is_arr(value)) {
        JVal *array = yyjson_mut_arr(json->doc);
        yyjson_val *child;
        size_t index, count;
        yyjson_arr_foreach((yyjson_val *)value, index, count, child) {
            JVal *v = copy_read(json, child, depth + 1);
            if (!v || !yyjson_mut_arr_append(array, v))
                return NULL;
        }
        return array;
    }
    return yyjson_val_mut_copy(json->doc, value);
}
Json *json_parse(const char *bytes, size_t len) {
    yyjson_doc *read = yyjson_read(bytes, len, 0);
    if (!read)
        return NULL;
    Json *json = new_doc();
    if (json) {
        json->root = copy_read(json, yyjson_doc_get_root(read), 0);
        if (!json->root) {
            json_free(json);
            json = NULL;
        }
    }
    yyjson_doc_free(read);
    return json;
}
Json *json_literal(const char *text) {
    return json_parse(text, strlen(text));
}
Json *json_clone(const JVal *value) {
    if (!value)
        return NULL;
    Json *json = new_doc();
    if (json) {
        json->root = yyjson_mut_val_mut_copy(json->doc, value);
        if (!json->root) {
            json_free(json);
            json = NULL;
        }
    }
    return json;
}
void json_free(Json *json) {
    if (json) {
        yyjson_mut_doc_free(json->doc);
        free(json);
    }
}
JVal *json_get(const JVal *obj, const char *key) {
    if (!yyjson_mut_is_obj(obj))
        return NULL;
    /* nlohmann kept the last occurrence of a duplicate key. */
    JVal *result = NULL, *k, *v;
    size_t i, n;
    yyjson_mut_obj_foreach((JVal *)obj, i, n, k, v) {
        if (yyjson_mut_equals_str(k, key))
            result = v;
    }
    return result;
}
JVal *json_at(const JVal *array, size_t index) {
    return yyjson_mut_arr_get(array, index);
}
const char *json_str(const JVal *v) {
    const char *s = yyjson_mut_get_str(v);
    return s ? s : "";
}
const char *json_string(const JVal *obj, const char *key, const char *fallback) {
    JVal *v = json_get(obj, key);
    return yyjson_mut_is_str(v) ? json_str(v) : fallback;
}
size_t json_len(const JVal *v) {
    return yyjson_mut_get_len(v);
}
bool json_is_object(const JVal *v) {
    return yyjson_mut_is_obj(v);
}
bool json_is_array(const JVal *v) {
    return yyjson_mut_is_arr(v);
}
bool json_is_string(const JVal *v) {
    return yyjson_mut_is_str(v);
}
bool json_is_int(const JVal *v) {
    return yyjson_mut_is_int(v);
}
bool json_is_number(const JVal *v) {
    return yyjson_mut_is_num(v);
}
bool json_is_bool(const JVal *v) {
    return yyjson_mut_is_bool(v);
}
bool json_is_null(const JVal *v) {
    return !v || yyjson_mut_is_null(v);
}
int64_t json_integer(const JVal *v, int64_t fallback) {
    if (!json_is_int(v))
        return fallback;
    if (yyjson_mut_is_uint(v) && yyjson_mut_get_uint(v) > INT64_MAX)
        return fallback;
    return yyjson_mut_get_sint(v);
}
int64_t json_int(const JVal *obj, const char *key, int64_t fallback) {
    return json_integer(json_get(obj, key), fallback);
}
double json_number(const JVal *obj, const char *key, double fallback) {
    JVal *v = json_get(obj, key);
    return json_is_number(v) ? yyjson_mut_get_num(v) : fallback;
}
bool json_bool(const JVal *obj, const char *key, bool fallback) {
    JVal *v = json_get(obj, key);
    return json_is_bool(v) ? yyjson_mut_get_bool(v) : fallback;
}
bool json_equal(const JVal *a, const JVal *b) {
    return yyjson_mut_equals(a, b);
}
JVal *json_new_object(Json *json) {
    if (!json)
        return NULL;
    JVal *v = yyjson_mut_obj(json->doc);
    if (!v)
        json->failed = true;
    return v;
}
JVal *json_new_array(Json *json) {
    if (!json)
        return NULL;
    JVal *v = yyjson_mut_arr(json->doc);
    if (!v)
        json->failed = true;
    return v;
}
int json_add(Json *json, JVal *obj, const char *key, JVal *value) {
    if (!json)
        return -1;
    JVal *k = yyjson_mut_strcpy(json->doc, key);
    if (!k || !value || !yyjson_mut_obj_put(obj, k, value)) {
        json->failed = true;
        return -1;
    }
    return 0;
}
int json_add_string(Json *json, JVal *obj, const char *key, const char *text, size_t len) {
    return json_add(json, obj, key, json ? yyjson_mut_strncpy(json->doc, text, len) : NULL);
}
int json_set_stringn(Json *json, const char *key, const char *s, size_t n) {
    return json_add_string(json, JV(json), key, s, n);
}
int json_set_string(Json *json, const char *key, const char *s) {
    return json_set_stringn(json, key, s, strlen(s));
}
int json_set_int(Json *json, const char *key, int64_t v) {
    return json_add(json, JV(json), key, json ? yyjson_mut_sint(json->doc, v) : NULL);
}
int json_set_bool(Json *json, const char *key, bool v) {
    return json_add(json, JV(json), key, json ? yyjson_mut_bool(json->doc, v) : NULL);
}
int json_set_null(Json *json, const char *key) {
    return json_add(json, JV(json), key, json ? yyjson_mut_null(json->doc) : NULL);
}
int json_set_value(Json *json, const char *key, const JVal *v) {
    return json_add(json, JV(json), key, json && v ? yyjson_mut_val_mut_copy(json->doc, v) : NULL);
}
int json_append(Json *json, const JVal *v) {
    if (!json)
        return -1;
    JVal *copy = v ? yyjson_mut_val_mut_copy(json->doc, v) : NULL;
    if (!copy || !yyjson_mut_arr_append(json->root, copy)) {
        json->failed = true;
        return -1;
    }
    return 0;
}
int json_append_string(Json *json, const char *s) {
    if (!json)
        return -1;
    JVal *v = yyjson_mut_strcpy(json->doc, s);
    if (!v || !yyjson_mut_arr_append(json->root, v)) {
        json->failed = true;
        return -1;
    }
    return 0;
}
int json_remove_first(Json *json) {
    return json && yyjson_mut_arr_remove(json->root, 0) ? 0 : -1;
}
char *json_dump_value(const JVal *value, bool pretty, size_t *len) {
    /* Like nlohmann's error_handler::replace, malformed raw output becomes U+FFFD. */
    yyjson_write_flag flags = 0;
    if (pretty)
        flags |= YYJSON_WRITE_PRETTY_TWO_SPACES;
    char *output = yyjson_mut_val_write(value, flags, len);
    if (!output)
        output = yyjson_mut_val_write(
            value, flags | YYJSON_WRITE_ALLOW_INVALID_UNICODE | YYJSON_WRITE_ESCAPE_UNICODE, len);
    return output;
}
char *json_dump(const Json *json, bool pretty, size_t *len) {
    return json && !json->failed ? json_dump_value(json->root, pretty, len) : NULL;
}
Json *json_event(const char *type) {
    Json *json = json_object();
    json_set_string(json, "type", type);
    return json;
}
