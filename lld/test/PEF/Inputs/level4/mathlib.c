// Test Level 4.1: Math Library Implementation

#include "mathlib.h"

int add(int a, int b) {
    return a + b;
}

int subtract(int a, int b) {
    return a - b;
}

int multiply(int a, int b) {
    return a * b;
}

int divide(int a, int b) {
    if (b == 0) return 0;
    return a / b;
}

long factorial(int n) {
    long result = 1;
    int i;
    for (i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

int is_prime(int n) {
    int i;
    if (n < 2) return 0;
    for (i = 2; i * i <= n; i++) {
        if (n % i == 0) return 0;
    }
    return 1;
}
