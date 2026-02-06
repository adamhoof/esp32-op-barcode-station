#pragma once

#include <cstdint>

struct ProductData {
    char name[100];
    char unitOfMeasure[20];
    double price;
    uint16_t stock;
    double unitCoef;
    bool valid;
};
