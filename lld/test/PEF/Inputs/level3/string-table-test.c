// Test Level 3.2: String Table and Pointers
// Tests string literals, pointer initialization, complex const data

#include <string.h>

// String literals (should go to .rodata)
const char* messages[] = {
    "System 7.5",
    "System 8.0",
    "System 9.0",
    "Mac OS X"
};

// Pointer to data
int* global_ptr = 0;
const char* name_ptr = "Classic Mac OS";

// Struct with pointers
struct Config {
    const char* name;
    int version;
    const char* description;
};

const struct Config config = {
    "Toolbox",
    9,
    "Classic Mac OS Toolbox"
};

int main(void) {
    int local = 42;
    global_ptr = &local;

    // Test string operations
    size_t len = strlen(messages[0]);

    // Verify strlen
    if (len != 10) return 1;  // "System 7.5" is 10 chars

    // Test config struct
    if (config.version != 9) return 2;

    // Test string comparison
    if (strcmp(config.name, "Toolbox") != 0) return 3;

    return 0; // Success
}
