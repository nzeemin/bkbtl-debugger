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

//////////////////////////////////////////////////////////////////////
