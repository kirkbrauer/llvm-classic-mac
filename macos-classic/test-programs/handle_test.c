//===-- handle_test.c - Mac OS Classic Handle Memory Test -------*- C -*-===//
//
// Test program for Mac OS 9 LLVM toolchain - Handle memory management
//
// This program demonstrates:
// 1. Handle lifecycle (NewHandle, DisposeHandle, HLock/HUnlock)
// 2. Handle size operations (GetHandleSize, SetHandleSize)
// 3. Memory queries (FreeMem, MaxBlock, StackSpace)
// 4. Handle duplication (HandToHand, PtrToHand)
// 5. Error checking with MemError()
//
// Expected behavior (6 phases):
// - Phase 1: Entry beep
// - Phase 2: Basic lifecycle - 2 short beeps (success) or 5 rapid beeps (fail)
// - Phase 3: Size operations - 3 short beeps (success) or 5 rapid beeps (fail)
// - Phase 4: Memory queries - 1 long beep (success) or 5 rapid beeps (fail)
// - Phase 5: Handle duplication - 4 short beeps (success) or 5 rapid beeps (fail)
// - Phase 6: Completion beep
//
//===----------------------------------------------------------------------===//

#include <MacMemory.h>
#include <Sound.h>
#include <OSUtils.h>

#define TEST_MAGIC_1  0x12345678
#define TEST_MAGIC_2  0xABCDEF00
#define TEST_MAGIC_3  0xDEADBEEF

typedef struct {
    long magic1;
    long magic2;
    long magic3;
} TestData;

int main(void) {
    Handle h, h2, h3;
    TestData *dataPtr;
    TestData stackData;
    Size size;
    long free, maxBlock, stack;
    int i;

    // Phase 1: Entry signal
    SysBeep(20);
    Delay(30, 0);

    // Phase 2: Basic lifecycle test
    h = NewHandle(sizeof(TestData));
    if (MemError() == noErr && h != 0) {
        HLock(h);
        dataPtr = (TestData*)*h;
        dataPtr->magic1 = TEST_MAGIC_1;
        dataPtr->magic2 = TEST_MAGIC_2;
        dataPtr->magic3 = TEST_MAGIC_3;
        if (dataPtr->magic1 == TEST_MAGIC_1 && dataPtr->magic2 == TEST_MAGIC_2) {
            HUnlock(h);
            DisposeHandle(h);
            // Success: 2 short beeps
            SysBeep(10); Delay(20, 0);
            SysBeep(10); Delay(20, 0);
        } else {
            // Failure: 5 rapid beeps
            HUnlock(h); DisposeHandle(h);
            for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
        }
    } else {
        for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
    }

    // Phase 3: Size operations test
    h = NewHandle(sizeof(TestData));
    if (MemError() == noErr && h != 0) {
        size = GetHandleSize(h);
        if (size == sizeof(TestData)) {
            HLock(h);
            dataPtr = (TestData*)*h;
            dataPtr->magic1 = TEST_MAGIC_1;
            HUnlock(h);
            SetHandleSize(h, sizeof(TestData) * 2);
            if (MemError() == noErr && GetHandleSize(h) == sizeof(TestData) * 2) {
                HLock(h);
                if (((TestData*)*h)->magic1 == TEST_MAGIC_1) {
                    HUnlock(h);
                    // Success: 3 short beeps
                    for (i = 0; i < 3; i++) { SysBeep(10); Delay(15, 0); }
                } else {
                    HUnlock(h);
                    for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
                }
            } else {
                for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
            }
        } else {
            for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
        }
        DisposeHandle(h);
    } else {
        for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
    }

    // Phase 4: Memory query test
    free = FreeMem();
    maxBlock = MaxBlock();
    stack = StackSpace();
    if (free > 0 && maxBlock > 0 && stack > 0 && maxBlock <= free) {
        // Success: 1 long beep
        SysBeep(40);
        Delay(30, 0);
    } else {
        for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
    }

    // Phase 5: Handle duplication test
    h = NewHandle(sizeof(TestData));
    if (MemError() == noErr && h != 0) {
        HLock(h);
        ((TestData*)*h)->magic1 = TEST_MAGIC_2;
        HUnlock(h);
        h2 = h;
        HandToHand(&h2);
        if (MemError() == noErr && h2 != 0) {
            HLock(h2);
            if (((TestData*)*h2)->magic1 == TEST_MAGIC_2) {
                HUnlock(h2);
                stackData.magic1 = TEST_MAGIC_3;
                h3 = 0;
                PtrToHand(&stackData, &h3, sizeof(TestData));
                if (MemError() == noErr && h3 != 0) {
                    HLock(h3);
                    if (((TestData*)*h3)->magic1 == TEST_MAGIC_3) {
                        HUnlock(h3);
                        // Success: 4 short beeps
                        for (i = 0; i < 4; i++) { SysBeep(8); Delay(12, 0); }
                    } else {
                        HUnlock(h3);
                        for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
                    }
                    DisposeHandle(h3);
                } else {
                    for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
                }
            } else {
                HUnlock(h2);
                for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
            }
            DisposeHandle(h2);
        } else {
            for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
        }
        DisposeHandle(h);
    } else {
        for (i = 0; i < 5; i++) { SysBeep(5); Delay(10, 0); }
    }

    // Phase 6: Completion signal
    Delay(30, 0);
    SysBeep(40);
    Delay(60, 0);

    return 0;
}
