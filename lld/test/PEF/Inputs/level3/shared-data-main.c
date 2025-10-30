// Test Level 3.3: Shared Data Main
// Uses shared variables and functions from module

#include "shared-data.h"

int main(void) {
    // Increment counter 3 times
    increment_counter();
    increment_counter();
    increment_counter();

    // Set new name
    set_name("Main");

    // Get final count (should be 3)
    int count = get_counter();

    // Return 0 if count is correct, 1 otherwise
    return (count == 3) ? 0 : 1;
}
