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
int main(int argc, char* argv[])
{
    // Console output mode
    std::setlocale(LC_ALL, "");
    std::wcout.imbue(std::locale(""));

    std::vector<std::wstring> wargs;
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    for (int argn = 1; argn < argc; argn++)
    {
        std::wstring warg = converter.from_bytes(argv[argn]);
        wargs.push_back(warg);
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
