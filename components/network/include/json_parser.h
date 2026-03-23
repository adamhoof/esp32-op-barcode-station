#pragma once

#include "product_data.h"
#include <cstring>

bool parse_product_json(const char* json_str, size_t len, ProductData *out_data);
