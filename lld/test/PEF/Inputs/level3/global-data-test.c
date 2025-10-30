// Test Level 3.1: Complex Global Data
// Tests all data section types and initialization patterns

// Test initialized data (.data section)
int global_int = 42;
long global_long = 0x12345678;
char global_string[] = "Hello, Classic Mac!";
short global_array[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

// Test uninitialized data (.bss section)
int uninitialized_int;
char buffer[1024];
long large_array[256];

// Test read-only data (.rodata section)
const int const_int = 100;
const char* const_string = "Read-only string";
const struct {
    short x;
    short y;
} const_point = {10, 20};

// Test static data
static int static_counter = 0;
static const char* static_messages[] = {
    "Message 1",
    "Message 2",
    "Message 3"
};

int main(void) {
    // Modify initialized globals
    global_int += 10;

    // Initialize BSS variables
    uninitialized_int = 99;
    buffer[0] = 'A';
    large_array[0] = 0xDEADBEEF;

    // Increment static
    static_counter++;

    // Use const data
    int result = global_int + const_int + const_point.x;

    // Verify global_array
    int sum = 0;
    int i;
    for (i = 0; i < 10; i++) {
        sum += global_array[i];
    }

    // Should return 52 + 55 = 107 (42+10+100 + sum of 1-10)
    return result + sum;
}
