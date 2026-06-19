// console.h
//
// Console color helpers. Used to highlight values that changed since the
// last step/run (e.g. in "regs"/"memory" output) the way the GUI debugger
// shows them in red.
//
// Platform behavior:
//   - Windows (_MSC_VER): uses SetConsoleTextAttribute on the handle
//     captured by Console_ColorInit().
//   - Linux/macOS: uses ANSI escape codes written directly to std::wcout.
// On both platforms, color is suppressed automatically when stdout is not
// a terminal (e.g. redirected to a file or piped), so redirected output
// never contains raw escape codes or has unexpected behavior.

#pragma once

//////////////////////////////////////////////////////////////////////

// Call once at startup, before any colored output. Captures the console
// handle on Windows and detects whether stdout is a terminal.
void Console_ColorInit();

void Console_ColorPrompt();

// Set the foreground color used to highlight a changed/modified value.
// Call Console_ColorReset() afterwards to return to normal text.
void Console_ColorModified(bool modified = true);

// Reset text color back to the console's default.
void Console_ColorReset();


//////////////////////////////////////////////////////////////////////
