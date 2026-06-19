// bkdecmd.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "stdafx.h"
#include <clocale>

#include "bkbtldebug.h"
#include "Emulator.h"
#include "emubase/Emubase.h"
#include "commands.h"
#include "util/console.h"


//////////////////////////////////////////////////////////////////////
// Preliminary function declarations

int wmain_impl(std::vector<std::wstring>& wargs);

void PrintWelcome();
void PrintUsage();
bool ParseCommandLine(std::vector<std::wstring>& wargs);


//////////////////////////////////////////////////////////////////////
// Globals

#define OPTIONCHAR L'/'
#define OPTIONSTR L"/"

// Machine configuration selected via "conf:NAME" on the command line;
// BK_CONF_BK0010_BASIC (BK-0010-BASIC) is the default.
BKConfiguration g_nSelectedConfiguration = BK_CONF_BK0010_BASIC;

struct ConfigurationNameStruct
{
    const wchar_t* name;
    BKConfiguration configuration;
};

const ConfigurationNameStruct ConfigurationNames[] =
{
    { L"BK-0010-BASIC", BK_CONF_BK0010_BASIC },
    { L"BK-0010-FOCAL", BK_CONF_BK0010_FOCAL },
    { L"BK-0010-FDD",   BK_CONF_BK0010_FDD },
    { L"BK-0011M",      BK_CONF_BK0011 },
    { L"BK-0011M-FDD",  BK_CONF_BK0011_FDD },
};
const size_t ConfigurationNamesCount = sizeof(ConfigurationNames) / sizeof(ConfigurationNames[0]);

// Portable case-insensitive wide string equality check (avoids relying on
// _wcsicmp, which is MSVC-only, or wcscasecmp, which isn't universally
// available either).
bool WStringEqualsIgnoreCase(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (towlower(a[i]) != towlower(b[i]))
            return false;
    }
    return true;
}

// Look up a configuration by name (case-insensitive). Returns true and
// fills *pConfiguration on success.
bool FindConfigurationByName(const std::wstring& name, BKConfiguration* pConfiguration)
{
    for (size_t i = 0; i < ConfigurationNamesCount; i++)
    {
        if (WStringEqualsIgnoreCase(name, ConfigurationNames[i].name))
        {
            *pConfiguration = ConfigurationNames[i].configuration;
            return true;
        }
    }
    return false;
}

// Name for the currently selected configuration, for display purposes.
const wchar_t* GetConfigurationName(BKConfiguration configuration)
{
    for (size_t i = 0; i < ConfigurationNamesCount; i++)
    {
        if (ConfigurationNames[i].configuration == configuration)
            return ConfigurationNames[i].name;
    }
    return L"(unknown)";
}


//////////////////////////////////////////////////////////////////////


void PrintWelcome()
{
    std::wcout << L"BKBTL emulator console debugger [" << __DATE__ << " " << __TIME__ << "]";
    std::wcout << std::endl;
}

void PrintUsage()
{
    std::wcout << std::endl << L"Usage:" << std::endl
            << L"  conf:NAME      Select machine configuration; default is BK-0010-BASIC" << std::endl
            << L"                 Available: ";
    for (size_t i = 0; i < ConfigurationNamesCount; i++)
    {
        if (i > 0) std::wcout << L", ";
        std::wcout << ConfigurationNames[i].name;
    }
    std::wcout << std::endl
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
        else if (warg.compare(0, 5, L"conf:") == 0)
        {
            std::wstring confName = warg.substr(5);
            BKConfiguration configuration;
            if (!FindConfigurationByName(confName, &configuration))
            {
                std::wcout << L"Unknown configuration: " << confName << std::endl;
                return false;
            }
            g_nSelectedConfiguration = configuration;
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
    Console_ColorInit();

    PrintWelcome();

    if (!ParseCommandLine(wargs))
    {
        PrintUsage();
        return 255;
    }

    std::wcout << L"Configuration: " << GetConfigurationName(g_nSelectedConfiguration) << std::endl;

    if (!Emulator_Init())
    {
        std::wcout << L"Failed to initialize emulator." << std::endl;
        return 1;
    }

    if (!Emulator_InitConfiguration(g_nSelectedConfiguration))
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
