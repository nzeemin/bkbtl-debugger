// console.cpp

#include "stdafx.h"
#include "console.h"

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN   // Excludes heavy, rarely-used APIs (RPC, DDE, etc.)
#define NOMINMAX              // Prevents min/max macros from clashing with std::min/max
#include <Windows.h>
#else
#include <unistd.h>
#endif


//////////////////////////////////////////////////////////////////////


bool g_okColorEnabled = false;  // True if stdout is a terminal that supports color

#ifdef _MSC_VER
HANDLE g_hConsole = INVALID_HANDLE_VALUE;
WORD g_wDefaultAttributes = 0;  // Console's original attributes, to restore on reset
#endif


//////////////////////////////////////////////////////////////////////


void Console_ColorInit()
{
#ifdef _MSC_VER
    g_hConsole = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hConsole == INVALID_HANDLE_VALUE || g_hConsole == NULL)
    {
        g_okColorEnabled = false;
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!::GetConsoleScreenBufferInfo(g_hConsole, &csbi))
    {
        // Not a real console (e.g. output redirected to a file) -- no color.
        g_okColorEnabled = false;
        return;
    }
    g_wDefaultAttributes = csbi.wAttributes;
    g_okColorEnabled = true;
#else
    // isatty(1) checks file descriptor 1, i.e. stdout -- whether it's
    // connected to a terminal or has been redirected/piped.
    g_okColorEnabled = (::isatty(1) != 0);
#endif
}

void Console_ColorPrompt()
{
    if (!g_okColorEnabled)
        return;

    // set cyan foreground
#ifdef _MSC_VER
    ::SetConsoleTextAttribute(g_hConsole, FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
#else
    std::wcout << L"\033[36m";
#endif
}

void Console_ColorModified(bool modified)
{
    if (!g_okColorEnabled)
        return;

    if (modified)
    {
        // set red foreground
#ifdef _MSC_VER
        ::SetConsoleTextAttribute(g_hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
#else
        std::wcout << L"\033[31m";
#endif
    }
    else
    {
        // reset color
#ifdef _MSC_VER
        ::SetConsoleTextAttribute(g_hConsole, g_wDefaultAttributes);
#else
        std::wcout << L"\033[0m";
#endif
    }
}

void Console_ColorReset()
{
    if (!g_okColorEnabled)
        return;

    // reset color
#ifdef _MSC_VER
    ::SetConsoleTextAttribute(g_hConsole, g_wDefaultAttributes);
#else
    std::wcout << L"\033[0m";
#endif
}

//////////////////////////////////////////////////////////////////////
