// bkdecmd.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "stdafx.h"
#include <clocale>

#include "bkbtldebug.h"
#include "Emulator.h"
#include "emubase/Emubase.h"
#include "commands.h"


//////////////////////////////////////////////////////////////////////
// Preliminary function declarations

int wmain_impl(std::vector<std::wstring>& wargs);

void PrintWelcome();
void PrintUsage();
bool ParseCommandLine(std::vector<std::wstring>& wargs);


//////////////////////////////////////////////////////////////////////
// Globals

#ifdef _MSC_VER
#define OPTIONCHAR L'/'
#define OPTIONSTR L"/"
#else
#define OPTIONCHAR L'-'
#define OPTIONSTR L"-"
#endif


//////////////////////////////////////////////////////////////////////


void PrintWelcome()
{
    std::wcout << L"BKBTL emulator console debugger [" << __DATE__ << " " << __TIME__ << "]";
    std::wcout << std::endl;
}

void PrintUsage()
{
    std::wcout << std::endl << L"Usage:" << std::endl
            << L"  TODO" << std::endl
            << L"  Options:" << std::endl
            << L"    " << OPTIONSTR << L"TODO" << std::endl;
}

bool ParseCommandLine(std::vector<std::wstring>& wargs)
{
    for (auto warg : wargs)
    {
        const wchar_t* arg = warg.c_str();
        if (arg[0] == OPTIONCHAR)
        {
            if (wcscmp(arg + 1, L"sha1") == 0)
            {
                //TODO
            }
            else
            {
                std::wcout << L"Unknown option: " << arg << std::endl;
                return false;
            }
        }
        else
        {
            {
                std::wcout << L"Unknown parameter: " << arg << std::endl;
                return false;
            }
        }
    }

    return true;
}

#ifdef _MSC_VER
int wmain(int argc, wchar_t* argv[])
{
    // Console output mode
    _setmode(_fileno(stdout), _O_U16TEXT);

    std::vector<std::wstring> wargs;
    for (int argn = 1; argn < argc; argn++)
    {
        wargs.push_back(std::wstring(argv[argn]));
    }

    return wmain_impl(wargs);
}
#else

// Minimal UTF-8 -> wstring decoder for command-line arguments, used instead
// of the now-deprecated <codecvt>/std::wstring_convert (which is also
// missing std::codecvt_utf8_utf16 entirely on libc++/macOS). wchar_t is
// 32-bit on Linux/macOS, so each decoded code point maps to a single
// wchar_t -- no surrogate pairs to worry about here (that's only a
// concern for the 16-bit wchar_t used on the _MSC_VER branch above, which
// doesn't go through this function at all).
std::wstring Utf8ToWString(const char* s)
{
    std::wstring result;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0)
    {
        unsigned char c = *p;
        uint32_t codepoint = 0;
        int extraBytes = 0;
        if ((c & 0x80) == 0x00)      { codepoint = c; extraBytes = 0; }
        else if ((c & 0xE0) == 0xC0) { codepoint = c & 0x1F; extraBytes = 1; }
        else if ((c & 0xF0) == 0xE0) { codepoint = c & 0x0F; extraBytes = 2; }
        else if ((c & 0xF8) == 0xF0) { codepoint = c & 0x07; extraBytes = 3; }
        else { p++; continue; }  // Invalid lead byte, skip it

        p++;
        bool okValid = true;
        for (int i = 0; i < extraBytes; i++)
        {
            if ((*p & 0xC0) != 0x80) { okValid = false; break; }  // Truncated/invalid sequence
            codepoint = (codepoint << 6) | (*p & 0x3F);
            p++;
        }
        if (okValid)
            result.push_back((wchar_t)codepoint);
    }
    return result;
}

int main(int argc, char* argv[])
{
    // Console output mode
    std::setlocale(LC_ALL, "");
    std::wcout.imbue(std::locale(""));

    std::vector<std::wstring> wargs;
    for (int argn = 1; argn < argc; argn++)
    {
        wargs.push_back(Utf8ToWString(argv[argn]));
    }

    return wmain_impl(wargs);
}
#endif

int wmain_impl(std::vector<std::wstring>& wargs)
{
    PrintWelcome();

    if (!ParseCommandLine(wargs))
    {
        PrintUsage();
        return 255;
    }

    if (!Emulator_Init())
    {
        std::wcout << L"Failed to initialize emulator." << std::endl;
        return 1;
    }

    if (!Emulator_InitConfiguration(BK_CONF_BK0010_BASIC))
    {
        std::wcout << L"Failed to initialize machine configuration." << std::endl;
        Emulator_Done();
        return 1;
    }

    std::wcout << L"Use 'h' command to show help." << std::endl;

    std::wstring line;
    for (;;)
    {
        PrintConsolePrompt();
        if (!std::getline(std::wcin, line))
            break;  // EOF (Ctrl+D / Ctrl+Z)

        if (!DoConsoleCommand(line))
            break;
    }

    Emulator_Done();

    std::wcout << std::endl << L"Done." << std::endl;
    return 0;
}


//////////////////////////////////////////////////////////////////////
