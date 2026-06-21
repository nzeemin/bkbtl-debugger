// commands.h
//
// Console command interpreter, ported from the table-driven command
// dispatcher in bkbtl/emulator/ConsoleView.cpp (the WinAPI GUI debugger),
// adapted for a stdin/stdout console loop.

#pragma once

#include "bkbtldebug.h"

//////////////////////////////////////////////////////////////////////

// Execute one console command line.
// Returns false if the command was "quit"/"exit" (caller should stop the loop).
bool DoConsoleCommand(const std::wstring& command);

// Print the command prompt, e.g. "100000> "
void PrintConsolePrompt();

// "examine"/"x" and "disasm"/"d"/"D" can leave a continuation armed after
// printing a page, so the caller can offer "press Enter for more" instead
// of going back to the normal prompt. See commands.cpp for details.

// True if a continuation is armed (the last command printed a page and
// there's more to show).
bool HasPendingContinuation();

// Print the next page for the armed continuation and re-arm for the page
// after that. Call this when the user responds to the "-- more --" prompt
// with an empty line (just Enter). Does nothing if none is armed.
void RunPendingContinuation();

// Clear the armed continuation without printing anything. Call this when
// the user responds to the "-- more --" prompt with anything other than
// an empty line. That input is just "stop paging" -- it answers the
// prompt, it is not a command line, so it should be discarded rather than
// dispatched via DoConsoleCommand (otherwise typing anything to get out
// of the pager could accidentally run an unintended command).
void ClearPendingContinuation();

//////////////////////////////////////////////////////////////////////
