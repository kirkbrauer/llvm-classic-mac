// Test Level 3.3: Shared Data Header
// Tests external symbol resolution and cross-module data access

#ifndef SHARED_DATA_H
#define SHARED_DATA_H

// External declarations
extern int shared_counter;
extern const char* shared_name;

// Function declarations
void increment_counter(void);
int get_counter(void);
void set_name(const char* name);

#endif // SHARED_DATA_H
