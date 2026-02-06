#pragma once

#include "print_message.h"
#include <cstring>

bool parse_product_json(const char* json_str, size_t len, ProductData *out_data);
