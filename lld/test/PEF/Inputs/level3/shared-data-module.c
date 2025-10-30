// Test Level 3.3: Shared Data Module
// Defines shared variables and functions used by main

#include "shared-data.h"

// Defined here, used in main
int shared_counter = 0;
const char* shared_name = "Module";

void increment_counter(void) {
    shared_counter++;
}

int get_counter(void) {
    return shared_counter;
}

void set_name(const char* name) {
    shared_name = name;
}
