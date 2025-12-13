//===-- simple_dialog_test.c - Button Test with Proper Event Loop --*- C -*-===//
//
// Test: Create window with button using correct Mac OS event handling patterns
// Based on EventTracker.c from Classic Mac C Compendium
//
//===----------------------------------------------------------------------===//

#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Memory.h>
#include <Sound.h>
#include <Events.h>
#include <NumberFormatting.h>

// Globals (following EventTracker.c pattern)
Boolean gDone;
WindowPtr gWindow;
ControlHandle gButton;

// Function prototypes
void ToolBoxInit(void);
void WindowInit(void);
void EventLoop(void);
void DoEvent(EventRecord *eventPtr);
void HandleMouseDown(EventRecord *eventPtr);

int main(void) {
    ToolBoxInit();
    WindowInit();
    EventLoop();
    return 0;
}

void ToolBoxInit(void) {
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nil);
    InitCursor();

    // DEBUG: 1 beep = Toolbox initialized
    SysBeep(10);
}

void WindowInit(void) {
    Rect bounds, buttonRect;

    // Create a simple window
    SetRect(&bounds, 100, 100, 400, 250);
    gWindow = NewCWindow(
        nil,
        &bounds,
        "\pButton Test",
        true,
        documentProc,
        (WindowPtr)-1,
        true,           // has close box
        0
    );

    if (!gWindow) {
        SysBeep(10); SysBeep(10); SysBeep(10); SysBeep(10); SysBeep(10);
        ExitToShell();
    }

    SetPort(gWindow);

    // Draw instructions
    MoveTo(20, 30);
    DrawString("\pClick the OK button to exit:");

    // Create button manually using NewControl
    SetRect(&buttonRect, 110, 80, 190, 100);
    gButton = NewControl(
        gWindow,
        &buttonRect,
        "\pOK",
        true,           // visible
        0,              // value
        0,              // min
        1,              // max
        0,              // pushButProc = 0
        0               // refCon
    );

    if (!gButton) {
        MoveTo(20, 60);
        DrawString("\pNewControl FAILED!");
        SysBeep(10); SysBeep(10); SysBeep(10);
    } else {
        // DEBUG: 2 beeps = Window and button created
        SysBeep(10); SysBeep(10);
    }

    ShowWindow(gWindow);
}

void EventLoop(void) {
    EventRecord event;

    gDone = false;
    while (gDone == false) {
        if (WaitNextEvent(everyEvent, &event, 0x7FFFFFFF, nil))
            DoEvent(&event);
    }
}

void DoEvent(EventRecord *eventPtr) {
    switch (eventPtr->what) {
        case mouseDown:
            HandleMouseDown(eventPtr);
            break;
        case updateEvt:
            BeginUpdate((WindowPtr)eventPtr->message);
            EndUpdate((WindowPtr)eventPtr->message);
            break;
    }
}

void HandleMouseDown(EventRecord *eventPtr) {
    WindowPtr window;
    long thePart;
    ControlHandle whichControl;
    Point localPt;
    Point mousePt;
    short controlPart;
    Str255 debugStr;
    Rect titleBarRect;

    // Copy Point
    mousePt.v = eventPtr->where.v;
    mousePt.h = eventPtr->where.h;

    // Get the window from FindWindow (the window pointer seems to work)
    thePart = FindWindow(mousePt, &window);

    // DEBUG: Draw coordinates and part on screen
    SetPort(gWindow);
    MoveTo(20, 120);
    NumToString(mousePt.h, debugStr);
    DrawString("\pH=");
    DrawString(debugStr);
    DrawString("\p V=");
    NumToString(mousePt.v, debugStr);
    DrawString(debugStr);
    DrawString("\p Part=");
    NumToString(thePart, debugStr);
    DrawString(debugStr);
    DrawString("\p   ");

    // WORKAROUND: Since FindWindow returns wrong part code but correct window,
    // manually determine part based on coordinates
    if (window == gWindow) {
        // Window bounds are (100, 100, 400, 250) in global coordinates
        // Title bar is approximately the top 20 pixels of the structure region
        // Structure region extends above content region by ~20 pixels for title bar

        // Title bar: roughly y=80 to y=100, x=100 to x=400
        // Close box: roughly y=80 to y=100, x=100 to x=120
        // Content: y=100 to y=250, x=100 to x=400

        if (mousePt.v < 100) {
            // Click is in title bar region
            if (mousePt.h < 120) {
                // Close box area (left side of title bar)
                MoveTo(20, 135);
                DrawString("\pClose box clicked!");
                gDone = true;
            } else {
                // Title bar - drag
                MoveTo(20, 135);
                DrawString("\pDragging...       ");
                DragWindow(window, mousePt, &qd.screenBits.bounds);
            }
        } else {
            // Content area
            MoveTo(20, 135);
            DrawString("\pContent area      ");

            // Convert to local coordinates for FindControl
            SetPort(window);
            localPt = mousePt;
            GlobalToLocal(&localPt);

            controlPart = FindControl(localPt, window, &whichControl);
            if (controlPart != 0 && whichControl == gButton) {
                if (TrackControl(whichControl, localPt, nil)) {
                    SysBeep(10);
                    gDone = true;
                }
            }
        }
    }
}
