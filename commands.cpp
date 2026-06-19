// commands.cpp
//
// Console command interpreter. This is a port of the table-driven command
// dispatcher (ConsoleCommandStruct / ConsoleCommands[]) from the WinAPI GUI
// debugger at bkbtl/emulator/ConsoleView.cpp, adapted to a plain stdin/stdout
// console loop instead of an Edit control.
//
// Commands implemented:
//   h                show help
//   r, regs          show all registers
//   rN               show register N (0..7)
//   rN=OOOOOO        set register N to octal value OOOOOO
//   rps              show PS (processor status word)
//   rps=OOOOOO       set PS to octal value OOOOOO
//   s                step into (execute one instruction)
//   so               step over (execute one instruction, stepping over CALL/JSR)
//   d, D             disassemble from PC (D = short format, no opcode words)
//   dOOOOOO, DOOOOOO disassemble from address OOOOOO
//   u                save memory dump to file memdump.bin
//   m                memory dump at current PC
//   mOOOOOO          memory dump at address OOOOOO
//   g                go; free run until a breakpoint is hit
//   gOOOOOO          go; run and stop at address OOOOOO
//   b                list all breakpoints
//   bOOOOOO          set breakpoint at address OOOOOO
//   bc               remove all breakpoints
//   bcOOOOOO         remove breakpoint at address OOOOOO
//   mo               type "M O" (BASIC) or "P SPACE M" (FOCAL) to exit to Monitor
//
// Not ported from the original ConsoleView.cpp:
//   w / wOOOOOO / wc / wcOOOOOO (watches) -- dropped; there is no watch-list
//     backend in this project's Emulator.cpp/.h (Emulator_AddWatch & co.
//     don't exist here), so these were left out rather than faked.
//   t / tN / tc (trace log) -- excluded per request (PRODUCT-gated in the
//     original anyway).

#include "stdafx.h"
#include "bkbtldebug.h"
#include "commands.h"
#include "Emulator.h"
#include "emubase/Emubase.h"

//////////////////////////////////////////////////////////////////////

const wchar_t* const MESSAGE_UNKNOWN_COMMAND  = L" Unknown command.";
const wchar_t* const MESSAGE_INVALID_REGNUM   = L" Invalid register number, 0..7 expected.";
const wchar_t* const MESSAGE_WRONG_VALUE      = L" Wrong value.";

CProcessor* GetCurrentProcessor()
{
    return g_pBoard->GetCPU();
}

// Forward declaration: RunUntilBreakpoint is defined further down (near the
// "go" commands) but is also used by Step Over.
void RunUntilBreakpoint();

// Print register name, octal value and binary value -- one line
void PrintRegisterLine(LPCTSTR strName, uint16_t value)
{
    TCHAR bufOctal[7];
    PrintOctalValue(bufOctal, value);
    TCHAR bufBinary[17];
    PrintBinaryValue(bufBinary, value);

    std::wcout << L"  " << strName << L" " << bufOctal << L"  " << bufBinary << std::endl;
}

// Print one disassembled instruction line.
// okShort: omit the raw opcode-word column (mirrors the "D" vs "d" command).
void PrintDisassembleLine(uint16_t address, uint16_t value, LPCTSTR instr, LPCTSTR args, bool okShort)
{
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, address);

    if (okShort)
    {
        std::wcout << L" " << bufAddr << L" " << instr << L" " << args << std::endl;
    }
    else
    {
        TCHAR bufValue[7];
        PrintOctalValue(bufValue, value);
        std::wcout << L" " << bufAddr << L" " << bufValue << L" " << instr << L" " << args << std::endl;
    }
}

// Disassemble instructions starting at address.
// okOneInstr: stop after the first instruction (used by Step Into).
// Returns the number of words in the last instruction disassembled.
int PrintDisassemble(CProcessor* pProc, uint16_t address, bool okOneInstr, bool okShort)
{
    bool okHaltMode = pProc->IsHaltMode();

    const int nWindowSize = 30;
    uint16_t memory[nWindowSize + 2];
    int addrtype;
    for (int i = 0; i < nWindowSize + 2; i++)
        memory[i] = g_pBoard->GetWordView((uint16_t)(address + i * 2), okHaltMode, true, &addrtype);

    int lastLength = 0;
    int length = 0;
    for (int index = 0; index < nWindowSize; index++)
    {
        uint16_t value = memory[index];

        if (length > 0)
        {
            // Continuation word(s) of the previous, longer instruction
        }
        else
        {
            if (okOneInstr && index > 0)
                break;

            TCHAR instr[8];
            TCHAR args[32];
            length = DisassembleInstruction(memory + index, address, instr, args);
            lastLength = length;
            if (index + length > nWindowSize)
                break;

            PrintDisassembleLine(address, value, instr, args, okShort);
        }

        length--;
        address += 2;
    }
    return lastLength;
}

// Print memory dump: address, 8 words in octal, then their ASCII representation
void PrintMemoryDump(const CProcessor* pProc, uint16_t address, int lines = 8)
{
    address &= ~1;  // Line up to even address
    bool okHaltMode = pProc->IsHaltMode();

    for (int line = 0; line < lines; line++)
    {
        uint16_t dump[8];
        int addrtype;
        for (int i = 0; i < 8; i++)
            dump[i] = g_pBoard->GetWordView((uint16_t)(address + i * 2), okHaltMode, false, &addrtype);

        TCHAR bufAddr[7];
        PrintOctalValue(bufAddr, address);
        std::wcout << L"  " << bufAddr << L"  ";

        for (int i = 0; i < 8; i++)
        {
            TCHAR bufValue[7];
            PrintOctalValue(bufValue, dump[i]);
            std::wcout << bufValue << L" ";
        }
        std::wcout << L" ";

        for (int i = 0; i < 8; i++)
        {
            uint16_t word = dump[i];
            uint8_t ch1 = (uint8_t)(word & 0xff);
            wchar_t wch1 = (ch1 < 32) ? L'\xB7' : (wchar_t)Translate_BK_Unicode(ch1);
            uint8_t ch2 = (uint8_t)(word >> 8);
            wchar_t wch2 = (ch2 < 32) ? L'\xB7' : (wchar_t)Translate_BK_Unicode(ch2);
            std::wcout << wch1 << wch2;
        }
        std::wcout << std::endl;

        address += 16;
    }
}

// Save full 64K memory dump to memdump.bin in the current directory.
// Ported from ConsoleView_SaveMemoryDump, using standard C++ file I/O
// instead of Win32 CreateFile/WriteFile.
bool SaveMemoryDump()
{
    std::vector<uint8_t> buf(65536);
    for (int i = 0; i < 65536; i++)
        buf[i] = g_pBoard->GetByte((uint16_t)i, true);

    std::ofstream file("memdump.bin", std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    return file.good();
}

//////////////////////////////////////////////////////////////////////
// Console command parameters -- filled in by the pattern matcher,
// consumed by the command callback. Mirrors ConsoleCommandParams.

struct ConsoleCommandParams
{
    std::wstring commandText;
    int paramReg1 = -1;
    uint16_t paramOct1 = 0;
    uint16_t paramOct2 = 0;
};

//////////////////////////////////////////////////////////////////////
// Console command handlers
// Mirrors the ConsoleView_Cmd* functions.

void CmdShowHelp(const ConsoleCommandParams& /*params*/)
{
    std::wcout <<
        L"Console command list:\n"
        L"  d              Disassemble from PC; use D for short format\n"
        L"  dOOOOOO        Disassemble from address OOOOOO\n"
        L"  g              Go; free run\n"
        L"  gOOOOOO        Go; run and stop at address OOOOOO\n"
        L"  h              Show this help\n"
        L"  m              Memory dump at current address\n"
        L"  mOOOOOO        Memory dump at address OOOOOO\n"
        L"  mo             Type M O (BASIC) or P SPACE M (FOCAL) to exit to Monitor\n"
        L"  r, regs        Show register values\n"
        L"  rN             Show value of register N; N=0..7\n"
        L"  rN=OOOOOO      Set register N to value OOOOOO; N=0..7\n"
        L"  rps            Show PS (processor status word)\n"
        L"  rps=OOOOOO     Set PS to value OOOOOO\n"
        L"  s              Step Into; executes one instruction\n"
        L"  so             Step Over; executes and stops after the current instruction\n"
        L"  b              List all breakpoints\n"
        L"  bOOOOOO        Set breakpoint at address OOOOOO\n"
        L"  bc             Remove all breakpoints\n"
        L"  bcOOOOOO       Remove breakpoint at address OOOOOO\n"
        L"  u              Save memory dump to file memdump.bin\n"
        L"  q, quit, exit  Quit the debugger\n";
}

void CmdPrintAllRegisters(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    for (int r = 0; r < 8; r++)
    {
        LPCTSTR name = REGISTER_NAME[r];
        uint16_t value = pProc->GetReg(r);
        PrintRegisterLine(name, value);
    }
    PrintRegisterLine(_T("PS"), pProc->GetPSW());
}

void CmdPrintRegister(const ConsoleCommandParams& params)
{
    int r = params.paramReg1;
    LPCTSTR name = REGISTER_NAME[r];
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t value = pProc->GetReg(r);
    PrintRegisterLine(name, value);
}

void CmdSetRegisterValue(const ConsoleCommandParams& params)
{
    int r = params.paramReg1;
    uint16_t value = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    pProc->SetReg(r, value);
}

void CmdPrintRegisterPSW(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t value = pProc->GetPSW();
    PrintRegisterLine(_T("PS"), value);
}

void CmdSetRegisterPSW(const ConsoleCommandParams& params)
{
    uint16_t value = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    pProc->SetPSW(value);
}

void CmdStepInto(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    PrintDisassemble(pProc, pProc->GetPC(), true, false);
    g_pBoard->DebugTicks();
}

void CmdStepOver(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    int instrLength = PrintDisassemble(pProc, pProc->GetPC(), true, false);

    int addrtype;
    uint16_t instr = g_pBoard->GetWordView(pProc->GetPC(), pProc->IsHaltMode(), true, &addrtype);

    // For JMP and BR use Step Into logic, not Step Over -- there's no
    // "next instruction" to break on, since control may not return here.
    if ((instr & ~(uint16_t)0077) == PI_JMP || (instr & ~(uint16_t)0377) == PI_BR)
    {
        g_pBoard->DebugTicks();
        return;
    }

    uint16_t bpaddress = (uint16_t)(pProc->GetPC() + instrLength * 2);
    Emulator_SetTempCPUBreakpoint(bpaddress);
    RunUntilBreakpoint();
}

void CmdPrintDisassembleAtPC(const ConsoleCommandParams& params)
{
    bool okShort = (params.commandText[0] == L'D');
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t address = pProc->GetPC();
    PrintDisassemble(pProc, address, false, okShort);
}

void CmdPrintDisassembleAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    bool okShort = (params.commandText[0] == L'D');
    CProcessor* pProc = GetCurrentProcessor();
    PrintDisassemble(pProc, address, false, okShort);
}

void CmdSaveMemoryDump(const ConsoleCommandParams& /*params*/)
{
    if (!SaveMemoryDump())
        std::wcout << L" Failed to save memory dump." << std::endl;
}

void CmdPrintMemoryDumpAtPC(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t address = pProc->GetPC();
    PrintMemoryDump(pProc, address);
}

void CmdPrintMemoryDumpAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    PrintMemoryDump(pProc, address);
}

// Run until a breakpoint is hit. Since this console has no message loop /
// timer driving frames the way the GUI build does, "go" here just drives
// Emulator_SystemFrame() in a blocking loop until it reports a breakpoint.
//
// Safety valve: there is no way to interrupt this from another thread (no
// GUI Stop button, no Ctrl+C handler), so a free run with no breakpoint
// ever set -- or a breakpoint at an address the CPU never reaches -- would
// hang the console forever. MAX_RUN_FRAMES caps it; at 25 frames/sec this
// is a generous ~2 minutes of emulated time before giving up.
const int MAX_RUN_FRAMES = 3000;

void RunUntilBreakpoint()
{
    Emulator_Start();
    int frames = 0;
    bool hitBreakpoint = false;
    while (g_okEmulatorRunning)
    {
        if (!Emulator_SystemFrame())
        {
            hitBreakpoint = true;
            Emulator_Stop();
            break;
        }
        frames++;
        if (frames >= MAX_RUN_FRAMES)
        {
            Emulator_Stop();
            break;
        }
    }
    CProcessor* pProc = GetCurrentProcessor();
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, pProc->GetPC());
    if (hitBreakpoint)
        std::wcout << L" Stopped at " << bufAddr << std::endl;
    else
        std::wcout << L" Stopped at " << bufAddr << L" (no breakpoint hit after "
                    << MAX_RUN_FRAMES << L" frames -- giving up; check your breakpoint address)" << std::endl;
}

void CmdRun(const ConsoleCommandParams& /*params*/)
{
    RunUntilBreakpoint();
}

//////////////////////////////////////////////////////////////////////
// "mo" -- jump to Monitor
//
// The GUI build does this by injecting keystrokes into the running screen
// view (ScreenView_KeyEvent), relying on its always-on timer loop to drain
// the key queue and let the BK's keyboard ISR see them. There is no screen
// view here, but Emulator_KeyEvent / Emulator_ProcessKeyEvent are the same
// underlying queue, so we drive a handful of frames ourselves after each
// keypress/keyrelease to get the same effect.

// Run a few emulator frames -- just enough for a queued key event to be
// drained by Emulator_ProcessKeyEvent() and processed by the keyboard ISR.
// Not a debugging "run": breakpoints are intentionally ignored here, since
// typing a Monitor command shouldn't be derailed by a CPU breakpoint.
void PumpFrames(int count)
{
    for (int i = 0; i < count; i++)
        g_pBoard->SystemFrame();
}

// Press and release one BK keyboard scancode, pumping frames around each
// half so the queued event actually gets consumed.
void TypeKey(uint8_t scancode)
{
    Emulator_KeyEvent(scancode, true, false);
    PumpFrames(2);
    Emulator_KeyEvent(scancode, false, false);
    PumpFrames(2);
}

// BK keyboard scancodes (octal), same values as the GUI's ConsoleView_GotoMonitor
const uint8_t BK_KEY_M     = 0115;
const uint8_t BK_KEY_O     = 0117;
const uint8_t BK_KEY_P     = 0120;
const uint8_t BK_KEY_SPACE = 0040;

void GotoMonitor()
{
    if ((g_nEmulatorConfiguration & BK_COPT_ROM_BASIC) != 0)
    {
        TypeKey(BK_KEY_M);
        TypeKey(BK_KEY_O);
    }
    else if ((g_nEmulatorConfiguration & BK_COPT_ROM_FOCAL) != 0)
    {
        TypeKey(BK_KEY_P);
        TypeKey(BK_KEY_SPACE);
        TypeKey(BK_KEY_M);
    }
}

void CmdGotoMonitor(const ConsoleCommandParams& /*params*/)
{
    GotoMonitor();
}

void CmdRunToAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    Emulator_SetTempCPUBreakpoint(address);
    RunUntilBreakpoint();
}

void CmdPrintAllBreakpoints(const ConsoleCommandParams& /*params*/)
{
    const uint16_t* pbps = Emulator_GetCPUBreakpointList();
    if (pbps == nullptr || *pbps == 0177777)
    {
        std::wcout << L" No breakpoints." << std::endl;
    }
    else
    {
        while (*pbps != 0177777)
        {
            TCHAR bufAddr[7];
            PrintOctalValue(bufAddr, *pbps);
            std::wcout << L"  " << bufAddr << std::endl;
            pbps++;
        }
    }
}

void CmdSetBreakpointAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    bool result = Emulator_AddCPUBreakpoint(address);
    if (!result)
        std::wcout << L" Failed to add breakpoint." << std::endl;
}

void CmdRemoveBreakpointAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    bool result = Emulator_RemoveCPUBreakpoint(address);
    if (!result)
        std::wcout << L" Failed to remove breakpoint." << std::endl;
}

void CmdRemoveAllBreakpoints(const ConsoleCommandParams& /*params*/)
{
    Emulator_RemoveAllBreakpoints();
}

//////////////////////////////////////////////////////////////////////
// Command table
//
// IMPORTANT: as with the original, list more specific forms (with more
// parameters) before less specific ones, since matching stops at the
// first pattern that fits -- e.g. "r%d" must come before "r", and
// "bc%ho" must come before "bc" which must come before "b".

enum ConsoleCommandArgInfo
{
    ARGINFO_NONE,       // No parameters
    ARGINFO_REG,        // Register number 0..7
    ARGINFO_OCT,        // Octal value
    ARGINFO_REG_OCT,    // Register number, octal value
};

typedef void (*CONSOLE_COMMAND_CALLBACK)(const ConsoleCommandParams& params);

struct ConsoleCommandStruct
{
    const wchar_t* prefix;          // Fixed command prefix to match, e.g. L"r"
    ConsoleCommandArgInfo arginfo;  // What follows the prefix, if anything
    CONSOLE_COMMAND_CALLBACK callback;
};

const ConsoleCommandStruct ConsoleCommands[] =
{
    { L"h",     ARGINFO_NONE,    CmdShowHelp },

    { L"r",     ARGINFO_REG_OCT, CmdSetRegisterValue },          // rN=OOOOOO
    { L"r",     ARGINFO_REG,     CmdPrintRegister },              // rN
    { L"rps=",  ARGINFO_OCT,     CmdSetRegisterPSW },             // rps=OOOOOO
    { L"rps ",  ARGINFO_OCT,     CmdSetRegisterPSW },             // rps OOOOOO
    { L"rps",   ARGINFO_NONE,    CmdPrintRegisterPSW },           // rps
    { L"regs",  ARGINFO_NONE,    CmdPrintAllRegisters },          // regs
    { L"r",     ARGINFO_NONE,    CmdPrintAllRegisters },          // r

    { L"so",    ARGINFO_NONE,    CmdStepOver },
    { L"s",     ARGINFO_NONE,    CmdStepInto },

    { L"d",     ARGINFO_OCT,     CmdPrintDisassembleAtAddress },  // dOOOOOO
    { L"D",     ARGINFO_OCT,     CmdPrintDisassembleAtAddress },  // DOOOOOO
    { L"d",     ARGINFO_NONE,    CmdPrintDisassembleAtPC },       // d
    { L"D",     ARGINFO_NONE,    CmdPrintDisassembleAtPC },       // D

    { L"u",     ARGINFO_NONE,    CmdSaveMemoryDump },

    { L"mo",    ARGINFO_NONE,    CmdGotoMonitor },                // mo
    { L"m",     ARGINFO_OCT,     CmdPrintMemoryDumpAtAddress },   // mOOOOOO
    { L"m",     ARGINFO_NONE,    CmdPrintMemoryDumpAtPC },        // m

    { L"g",     ARGINFO_OCT,     CmdRunToAddress },               // gOOOOOO
    { L"g",     ARGINFO_NONE,    CmdRun },                        // g

    { L"bc",    ARGINFO_OCT,     CmdRemoveBreakpointAtAddress },  // bcOOOOOO
    { L"bc",    ARGINFO_NONE,    CmdRemoveAllBreakpoints },       // bc
    { L"b",     ARGINFO_OCT,     CmdSetBreakpointAtAddress },     // bOOOOOO
    { L"b",     ARGINFO_NONE,    CmdPrintAllBreakpoints },        // b
};

const size_t ConsoleCommandsCount = sizeof(ConsoleCommands) / sizeof(ConsoleCommands[0]);

// Try to match `command` against one table entry.
// On success, fills in `params` (paramReg1 / paramOct1) as needed and returns true.
bool MatchCommand(const std::wstring& command, const ConsoleCommandStruct& cmd, ConsoleCommandParams& params)
{
    const std::wstring prefix = cmd.prefix;
    if (command.compare(0, prefix.size(), prefix) != 0)
        return false;
    std::wstring rest = command.substr(prefix.size());

    switch (cmd.arginfo)
    {
    case ARGINFO_NONE:
        return rest.empty();

    case ARGINFO_REG:
        {
            if (rest.empty() || rest.size() > 1 || !iswdigit(rest[0]))
                return false;
            params.paramReg1 = rest[0] - L'0';
            return true;  // range-checked by caller (0..7 expected)
        }

    case ARGINFO_OCT:
        {
            if (rest.empty())
                return false;
            for (wchar_t ch : rest)
                if (ch < L'0' || ch > L'7') return false;
            uint16_t value = 0;
            for (wchar_t ch : rest)
                value = (uint16_t)((value << 3) + (ch - L'0'));
            params.paramOct1 = value;
            return true;
        }

    case ARGINFO_REG_OCT:
        {
            // Accept "N=OOOOOO" or "N OOOOOO"
            if (rest.empty() || !iswdigit(rest[0]))
                return false;
            params.paramReg1 = rest[0] - L'0';
            std::wstring tail = rest.substr(1);
            if (tail.empty() || (tail[0] != L'=' && tail[0] != L' '))
                return false;
            tail = tail.substr(1);
            if (tail.empty())
                return false;
            for (wchar_t ch : tail)
                if (ch < L'0' || ch > L'7') return false;
            uint16_t value = 0;
            for (wchar_t ch : tail)
                value = (uint16_t)((value << 3) + (ch - L'0'));
            params.paramOct1 = value;
            return true;
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////

void PrintConsolePrompt()
{
    CProcessor* pProc = GetCurrentProcessor();
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, pProc->GetPC());
    std::wcout << bufAddr << L"> ";
}

bool DoConsoleCommand(const std::wstring& command)
{
    if (command.empty())
        return true;  // Nothing to do, keep looping

    if (command == L"q" || command == L"quit" || command == L"exit")
        return false;

    ConsoleCommandParams params;
    params.commandText = command;

    bool parsedOkay = false, parseError = false;
    for (size_t i = 0; i < ConsoleCommandsCount; i++)
    {
        const ConsoleCommandStruct& cmd = ConsoleCommands[i];
        ConsoleCommandParams trialParams;
        trialParams.commandText = command;

        if (!MatchCommand(command, cmd, trialParams))
            continue;

        if ((cmd.arginfo == ARGINFO_REG || cmd.arginfo == ARGINFO_REG_OCT) &&
            (trialParams.paramReg1 < 0 || trialParams.paramReg1 > 7))
        {
            std::wcout << MESSAGE_INVALID_REGNUM << std::endl;
            parseError = true;
            break;
        }

        params = trialParams;
        cmd.callback(params);
        parsedOkay = true;
        break;
    }

    if (!parsedOkay && !parseError)
        std::wcout << MESSAGE_UNKNOWN_COMMAND << std::endl;

    return true;
}

//////////////////////////////////////////////////////////////////////
