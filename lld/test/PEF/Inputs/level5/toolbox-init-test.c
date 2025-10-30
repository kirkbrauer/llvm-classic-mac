// Test Level 5: Toolbox Initialization Test
// Tests classic Mac OS Toolbox initialization sequence

// Uses real Mac OS headers with CALL_NOT_IN_CARBON support
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>

// Note: Some Toolbox init functions like InitWindows, InitMenus, InitDialogs
// are not available in the headers even with CALL_NOT_IN_CARBON, so we
// declare them manually
extern void InitWindows(void);
extern void InitMenus(void);
extern void InitDialogs(void* restartProc);
extern void SysBeep(short duration);

int main(void) {
    // Classic Mac OS Toolbox initialization sequence
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    InitDialogs(0);
    InitCursor();

    // Make a beep to show we initialized successfully
    SysBeep(10);

    // Clean up event queue
    FlushEvents(everyEvent, 0);

    return 0;
}
