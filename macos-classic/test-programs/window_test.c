//===-- window_test.c - QuickDraw Window Test -------*- C -*-===//
//
// Test program for Mac OS 9 LLVM toolchain - QuickDraw window display
//
// This program demonstrates:
// 1. QuickDraw globals (qd structure) provided by macos_classic_qd.o
// 2. Toolbox initialization sequence (InitGraf, InitFonts, etc.)
// 3. Window creation with NewCWindow
// 4. Pascal string support (\p prefix)
// 5. Simple event polling with Button()
//
// Expected behavior:
// - Window appears with title "LLVM Test"
// - Window positioned 50 pixels from screen edges
// - Standard arrow cursor visible
// - Click mouse button to exit
//
//===----------------------------------------------------------------------===//

#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Events.h>

int main(void) {
    WindowPtr window;
    Rect windRect;

    // Initialize Toolbox managers in proper order
    // InitGraf MUST be called first before any other manager
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nil);
    InitCursor();

    // Create window bounds from screen, inset by 50 pixels
    windRect = qd.screenBits.bounds;
    InsetRect(&windRect, 50, 50);

    // Create color window with Pascal string title
    window = NewCWindow(
        nil,                  // Auto-allocate storage
        &windRect,            // Window bounds
        "\pLLVM Test",        // Pascal string title (tests \p prefix)
        true,                 // Visible immediately
        documentProc,         // Standard document window
        (WindowPtr)-1,        // Front window
        false,                // No close box
        0                     // No refCon
    );

    // Set as current graphics port
    SetPort(window);

    // Wait for mouse button click
    while (!Button());

    // Exit (ExitToShell called by C runtime)
    return 0;
}
