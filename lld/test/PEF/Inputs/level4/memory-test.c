// Test Level 4.2: Memory Manager Test
// Tests Mac OS Memory Manager APIs (NewPtr, NewHandle, etc.)

// Uses real Mac OS headers via automatic MacHeadersCompat.h inclusion
#include <MacTypes.h>
#include <MacMemory.h>

int main(void) {
    Ptr myPtr;
    Handle myHandle;
    OSErr err;

    // Test NewPtr
    myPtr = NewPtr(1024);
    err = MemError();
    if (err != 0 || myPtr == 0) {
        return 1; // NewPtr failed
    }

    // Test GetPtrSize
    long ptrSize = GetPtrSize(myPtr);
    if (ptrSize != 1024) {
        DisposePtr(myPtr);
        return 2; // GetPtrSize failed
    }

    // Dispose pointer
    DisposePtr(myPtr);

    // Test Handle allocation
    myHandle = NewHandle(2048);
    err = MemError();
    if (err != 0 || myHandle == 0) {
        return 3; // NewHandle failed
    }

    // Test GetHandleSize
    long handleSize = GetHandleSize(myHandle);
    if (handleSize != 2048) {
        DisposeHandle(myHandle);
        return 4; // GetHandleSize failed
    }

    // Dispose handle
    DisposeHandle(myHandle);

    return 0; // Success
}
