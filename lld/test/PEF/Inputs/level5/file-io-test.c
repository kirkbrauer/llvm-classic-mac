// Test Level 5: File I/O Test
// Tests Classic Mac OS file operations using stdio

#include <stdio.h>
#include <string.h>

int main(void) {
    FILE* fp;
    char buffer[256];
    const char* testData = "Hello from Classic Mac OS!\n";
    const char* filename = "test-output.txt";

    // Test file writing
    fp = fopen(filename, "w");
    if (fp == 0) {
        return 1; // Failed to open for writing
    }

    fprintf(fp, "%s", testData);
    fprintf(fp, "Line 2: Testing fprintf\n");
    fprintf(fp, "Line 3: Number = %d\n", 42);

    fclose(fp);

    // Test file reading
    fp = fopen(filename, "r");
    if (fp == 0) {
        return 2; // Failed to open for reading
    }

    int line_count = 0;
    while (fgets(buffer, sizeof(buffer), fp) != 0) {
        line_count++;
    }

    fclose(fp);

    // Should have read 3 lines
    if (line_count != 3) {
        return 3; // Wrong number of lines
    }

    return 0; // Success
}
