// Test Level 4.1: Math Library Test Program

#include "mathlib.h"

int main(void) {
    int test_failures = 0;

    // Test add
    if (add(10, 5) != 15) test_failures++;

    // Test subtract
    if (subtract(10, 5) != 5) test_failures++;

    // Test multiply
    if (multiply(10, 5) != 50) test_failures++;

    // Test divide
    if (divide(10, 5) != 2) test_failures++;

    // Test factorial (5! = 120)
    if (factorial(5) != 120) test_failures++;

    // Test is_prime
    if (!is_prime(17)) test_failures++;
    if (is_prime(18)) test_failures++;

    return test_failures;
}
