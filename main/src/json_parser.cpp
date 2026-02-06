#include "json_parser.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "jsmn.h"

static bool json_eq(const char *json, const jsmntok_t &tok, const char *s)
{
    if (tok.type != JSMN_STRING) return false;
    const int len = tok.end - tok.start;
    if (len != static_cast<int>(strlen(s))) return false;
    return memcmp(json + tok.start, s, len) == 0;
}

static void json_copy_val(const char *json, const jsmntok_t &tok, char *out, size_t max_len)
{
    const int len = tok.end - tok.start;
    if (len <= 0) {
        out[0] = '\0';
        return;
    }
    const size_t copy_len = static_cast<size_t>(len) < (max_len - 1) ? static_cast<size_t>(len) : (max_len - 1);
    memcpy(out, json + tok.start, copy_len);
    out[copy_len] = '\0';
}

bool parse_product_json(const char *json_str, size_t len, ProductData *out_data)
{
    jsmn_parser p;
    jsmn_init(&p);

    jsmntok_t t[32];

    int r = jsmn_parse(&p, json_str, len, t, sizeof(t) / sizeof(t[0]));

    if (r < 1 || t[0].type != JSMN_OBJECT) {
        return false;
    }

    memset(out_data, 0, sizeof(ProductData));

    char tmp_buf[32];

    for (int i = 1; i < r; i++) {
        if (json_eq(json_str, t[i], "name")) {
            json_copy_val(json_str, t[i+1], out_data->name, sizeof(out_data->name));
            i++;
        }
        else if (json_eq(json_str, t[i], "unitOfMeasure")) {
            json_copy_val(json_str, t[i+1], out_data->unitOfMeasure, sizeof(out_data->unitOfMeasure));
            i++;
        }
        else if (json_eq(json_str, t[i], "price")) {
            json_copy_val(json_str, t[i+1], tmp_buf, sizeof(tmp_buf));
            out_data->price = strtof(tmp_buf, nullptr);
            i++;
        }
        else if (json_eq(json_str, t[i], "stock")) {
            json_copy_val(json_str, t[i+1], tmp_buf, sizeof(tmp_buf));
            out_data->stock = static_cast<uint16_t>(strtol(tmp_buf, nullptr, 10));
            i++;
        }
        else if (json_eq(json_str, t[i], "unitOfMeasureCoef")) {
            json_copy_val(json_str, t[i+1], tmp_buf, sizeof(tmp_buf));
            out_data->unitCoef = strtof(tmp_buf, nullptr);
            i++;
        }
        else if (json_eq(json_str, t[i], "valid")) {
            if (json_str[t[i+1].start] == 't') {
                out_data->valid = true;
            } else {
                out_data->valid = false;
            }
            i++;
        }
    }

    return true;
}