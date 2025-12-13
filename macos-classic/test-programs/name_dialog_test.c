//===-- name_dialog_test.c - Modal Dialog + Event Loop Test -------*- C -*-===//
//
// Test program for Mac OS 9 LLVM toolchain demonstrating:
// 1. Programmatic dialog creation with NewDialog()
// 2. Modal dialog with text input (EditText item)
// 3. Full WaitNextEvent event loop
// 4. Window dragging, close box, update events
// 5. Text rendering from user input
//
//===----------------------------------------------------------------------===//

#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Events.h>
#include <Memory.h>
#include <Sound.h>

// Global storage for user's name
Str255 userName;
WindowPtr mainWindow;

// Function prototypes
void InitializeToolbox(void);
Handle CreateDialogItemList(void);
void ShowNameInputDialog(void);
WindowPtr CreateMainWindow(void);
void RunEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleMouseDown(EventRecord *event);
void HandleUpdate(EventRecord *event);
void DrawWindowContents(void);

int main(void) {
    // Initialize Mac OS Toolbox
    InitializeToolbox();

    // Show modal dialog to get user's name
    // (Window creation happens inside ShowNameInputDialog after dialog closes)
    ShowNameInputDialog();

    // Run event loop (exits via close box → ExitToShell)
    RunEventLoop();

    return 0;  // Never reached
}

void InitializeToolbox(void) {
    // CRITICAL: InitGraf MUST be called first
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nil);
    InitCursor();
}

Handle CreateDialogItemList(void) {
    Handle ditl;
    Size ditlSize;
    unsigned char *p;

    // Calculate size:
    // 2 bytes (count) + Item1(16) + Item2(14) + Item3(14) = 46 bytes
    ditlSize = 46;

    ditl = NewHandle(ditlSize);
    if (!ditl) return nil;

    HLock(ditl);
    p = (unsigned char *)*ditl;

    // Item count - 1 (3 items total, so write 2)
    *((short *)p) = 2;
    p += 2;

    // ----- Item 1: OK Button -----
    *((Handle *)p) = nil; p += 4;           // placeholder
    *((short *)p) = 110; p += 2;            // top
    *((short *)p) = 200; p += 2;            // left
    *((short *)p) = 130; p += 2;            // bottom
    *((short *)p) = 270; p += 2;            // right
    *p = 4; p++;                            // type = kButtonDialogItem
    *p = 2; p++;                            // pascal string length
    *p = 'O'; p++;
    *p = 'K'; p++;

    // ----- Item 2: UserItem (prompt area) -----
    *((Handle *)p) = nil; p += 4;
    *((short *)p) = 10; p += 2;
    *((short *)p) = 10; p += 2;
    *((short *)p) = 30; p += 2;
    *((short *)p) = 380; p += 2;
    *p = 0; p++;                            // type = kUserDialogItem
    *p = 0; p++;                            // no data

    // ----- Item 3: EditText field -----
    *((Handle *)p) = nil; p += 4;
    *((short *)p) = 40; p += 2;
    *((short *)p) = 20; p += 2;
    *((short *)p) = 56; p += 2;
    *((short *)p) = 360; p += 2;
    *p = 16; p++;                           // type = kEditTextDialogItem
    *p = 0; p++;                            // empty initial text

    HUnlock(ditl);
    return ditl;
}

void ShowNameInputDialog(void) {
    Handle ditl;
    DialogPtr dialog;
    Rect bounds;
    short itemHit;
    short itemType;
    Handle itemHandle;
    Rect itemBox;

    // DEBUG: 1 beep = starting ShowNameInputDialog
    SysBeep(10);

    // Create DITL
    ditl = CreateDialogItemList();
    if (!ditl) {
        // DEBUG: 5 beeps = DITL creation failed
        SysBeep(10); SysBeep(10); SysBeep(10); SysBeep(10); SysBeep(10);
        return;
    }

    // DEBUG: 2 beeps = DITL created OK
    SysBeep(10); SysBeep(10);

    // Position dialog in center of screen
    bounds = qd.screenBits.bounds;
    InsetRect(&bounds, 100, 150);  // Smaller than screen

    // Create dialog
    dialog = NewDialog(
        nil,                      // auto-allocate storage
        &bounds,                  // bounds rectangle
        "\pEnter Your Name",      // Pascal string title
        true,                     // visible immediately
        dBoxProc,                 // plain dialog box (proc ID = 1)
        (WindowPtr)-1,            // frontmost window
        false,                    // no go-away box
        0,                        // refCon
        ditl                      // item list handle
    );

    if (!dialog) {
        // DEBUG: 6 beeps = NewDialog failed
        SysBeep(10); SysBeep(10); SysBeep(10); SysBeep(10); SysBeep(10); SysBeep(10);
        DisposeHandle(ditl);
        return;
    }

    // DEBUG: 3 beeps = Dialog created OK, about to enter ModalDialog
    SysBeep(10); SysBeep(10); SysBeep(10);

    // Set keyboard focus to EditText field (item 3)
    // Select all text (0 to 32767 = entire text)
    SelectDialogItemText(dialog, 3, 0, 32767);

    // Modal dialog loop
    do {
        ModalDialog(nil, &itemHit);
        // Loop continues until OK button (item 1) is clicked
        // ModalDialog handles Return/Enter key as OK button click
    } while (itemHit != 1);

    // DEBUG: 4 beeps = ModalDialog returned with OK
    SysBeep(10); SysBeep(10); SysBeep(10); SysBeep(10);

    // Extract text from EditText field (item 3)
    GetDialogItem(dialog, 3, &itemType, &itemHandle, &itemBox);
    GetDialogItemText(itemHandle, userName);

    // Clean up
    DisposeDialog(dialog);      // Dispose dialog (but not DITL)
    DisposeHandle(ditl);        // Must manually dispose DITL

    // Now create the main window AFTER dialog is dismissed
    mainWindow = CreateMainWindow();
    if (mainWindow) {
        SetPort(mainWindow);
        DrawWindowContents();
    }
}

WindowPtr CreateMainWindow(void) {
    Rect windRect;
    WindowPtr window;

    // Create window bounds (inset from screen edges)
    windRect = qd.screenBits.bounds;
    InsetRect(&windRect, 50, 50);

    // Create color window WITH close box
    window = NewCWindow(
        nil,                      // auto-allocate storage
        &windRect,                // bounds rectangle
        "\pName Display",         // Pascal string title
        true,                     // visible immediately
        documentProc,             // standard document window (proc ID = 0)
        (WindowPtr)-1,            // frontmost window
        true,                     // HAS CLOSE BOX (goAwayFlag)
        0                         // refCon
    );

    return window;
}

void DrawWindowContents(void) {
    // Clear window background
    EraseRect(&qd.thePort->portRect);

    // Draw user's name
    MoveTo(20, 30);              // Position pen
    DrawString(userName);        // Draw Pascal string
}

void HandleUpdate(EventRecord *event) {
    WindowPtr window;
    GrafPtr oldPort;

    // Extract window from event message
    window = (WindowPtr)event->message;

    // Save current port
    GetPort(&oldPort);
    SetPort(window);

    // BeginUpdate/EndUpdate validate the update region
    BeginUpdate(window);
    DrawWindowContents();
    EndUpdate(window);

    // Restore port
    SetPort(oldPort);
}

void HandleMouseDown(EventRecord *event) {
    WindowPtr whichWindow;
    short partCode;

    // Determine which window and which part was clicked
    partCode = FindWindow(event->where, &whichWindow);

    switch (partCode) {
        case inDrag:
            // User clicked in title bar → drag window
            // qd.screenBits.bounds = drag constraint rect (screen bounds)
            DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
            break;

        case inGoAway:
            // User clicked close box
            // TrackGoAway provides visual feedback (highlights box)
            // Returns true if mouse released while still in close box
            if (TrackGoAway(whichWindow, event->where)) {
                ExitToShell();  // Exit application
            }
            break;

        case inContent:
            // User clicked in window content area
            // If window not frontmost, bring to front
            if (whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            }
            break;
    }
}

void HandleEvent(EventRecord *event) {
    switch (event->what) {
        case mouseDown:
            HandleMouseDown(event);
            break;

        case updateEvt:
            HandleUpdate(event);
            break;
    }
}

void RunEventLoop(void) {
    EventRecord event;
    Boolean gotEvent;

    // Infinite loop (exits via ExitToShell in close box handler)
    while (true) {
        // WaitNextEvent yields to system (cooperative multitasking)
        // everyEvent = 0xFFFF (all event types)
        // 0 = no sleep time (process events as fast as possible)
        // nil = no mouse region
        gotEvent = WaitNextEvent(everyEvent, &event, 0, nil);

        if (gotEvent) {
            HandleEvent(&event);
        }
    }
}
