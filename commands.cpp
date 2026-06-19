// commands.cpp
//
// Console command interpreter. This is a port of the table-driven command
// dispatcher (ConsoleCommandStruct / ConsoleCommands[]) from the WinAPI GUI
// debugger at bkbtl/emulator/ConsoleView.cpp, adapted to a plain stdin/stdout
// console loop instead of an Edit control.
//
// Not ported from the original ConsoleView.cpp:
//   w / wXXXXXX / wc / wcXXXXXX (watches) -- dropped; there is no watch-list
//     backend in this project's Emulator.cpp/.h (Emulator_AddWatch & co.
//     don't exist here), so these were left out rather than faked.
//   t / tN / tc (trace log) -- excluded per request (PRODUCT-gated in the
//     original anyway).

#include "stdafx.h"
#include <cstdlib>
#include <ctime>
#include <chrono>
#include "bkbtldebug.h"
#include "commands.h"
#include "Emulator.h"
#include "emubase/Emubase.h"
#include "util/BitmapFile.h"
#include "util/console.h"


//////////////////////////////////////////////////////////////////////


const wchar_t* const MESSAGE_UNKNOWN_COMMAND  = L" Unknown command.";
const wchar_t* const MESSAGE_INVALID_REGNUM   = L" Invalid register number, 0..7 expected.";
const wchar_t* const MESSAGE_WRONG_VALUE      = L" Wrong value.";

CProcessor* GetCurrentProcessor()
{
    return g_pBoard->GetCPU();
}

// Forward declaration: RunUntilBreakpoint is defined further down (near the
// "go" commands) but is also used by CmdStepOver defined before it.
void RunUntilBreakpoint(int maxFrames = 3000);

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
        uint16_t changed[8];
        int addrtype;
        for (int i = 0; i < 8; i++)
        {
            dump[i] = g_pBoard->GetWordView((uint16_t)(address + i * 2), okHaltMode, false, &addrtype);
            changed[i] = addrtype == ADDRTYPE_ROM
                ? 0
                : Emulator_GetChangeRamStatus(address + i * 2);
        }

        TCHAR bufAddr[7];
        PrintOctalValue(bufAddr, address);
        std::wcout << L"  " << bufAddr << L"  ";

        for (int i = 0; i < 8; i++)
        {
            if (changed[i] != 0) Console_ColorModified();
            TCHAR bufValue[7];
            PrintOctalValue(bufValue, dump[i]);
            std::wcout << bufValue << L" ";
            Console_ColorReset();
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

// Print memory dump: address, 16 bytes in octal (3 digits each), then their
// ASCII representation. Same layout as PrintMemoryDump but byte-granular --
// uses the same PrintOctalValue helper, taking only its 3 lowest digits
// (a byte never needs more than 3 octal digits: 000..377).
void PrintMemoryDumpBytes(const CProcessor* pProc, uint16_t address, int lines = 8)
{
    bool okHaltMode = pProc->IsHaltMode();

    for (int line = 0; line < lines; line++)
    {
        uint16_t dump[8];
        uint16_t changed[8];
        int addrtype;
        for (int i = 0; i < 8; i++)
        {
            dump[i] = g_pBoard->GetWordView((uint16_t)(address + i * 2), okHaltMode, false, &addrtype);
            changed[i] = addrtype == ADDRTYPE_ROM
                ? 0
                : Emulator_GetChangeRamStatus(address + i * 2);
        }

        TCHAR bufAddr[7];
        PrintOctalValue(bufAddr, address);
        std::wcout << L"  " << bufAddr << L"  ";

        for (int i = 0; i < 8; i++)
        {
            uint16_t value = dump[i];
            if (changed[i] != 0) Console_ColorModified();
            TCHAR bufValue[7];
            PrintOctalValue(bufValue, value & 0xFF);
            std::wcout << (bufValue + 3) << L" ";
            PrintOctalValue(bufValue, value >> 8);
            std::wcout << (bufValue + 3) << L" ";
            Console_ColorReset();
        }
        std::wcout << L" ";

        for (int i = 0; i < 8; i++)
        {
            uint16_t value = dump[i];
            uint8_t ch1 = (value >> 8);
            wchar_t wch1 = (ch1 < 32) ? L'\xB7' : (wchar_t)Translate_BK_Unicode(ch1);
            std::wcout << wch1;
            uint8_t ch2 = (value >> 8);
            wchar_t wch2 = (ch2 < 32) ? L'\xB7' : (wchar_t)Translate_BK_Unicode(ch2);
            std::wcout << wch2;
        }
        std::wcout << std::endl;

        address += 16;
    }
}

// Convert a wstring command-line argument (e.g. a filename) to a TCHAR
// string. Under GCC, TCHAR is plain char, so this is a narrowing
// conversion using the current C locale; under real MSVC, TCHAR is
// typically wchar_t already, so it's a no-op copy. Filenames here are
// expected to be plain ASCII, so the locale-dependent narrowing is not a
// practical concern.
std::basic_string<TCHAR> WStringToTString(const std::wstring& ws)
{
#ifdef _UNICODE
    return ws;
#else
    std::vector<char> buf(ws.size() * MB_CUR_MAX + 1);
    std::wcstombs(buf.data(), ws.c_str(), buf.size());
    return std::string(buf.data());
#endif
}

// Convert a wstring command-line argument to a plain std::string, always
// narrow regardless of TCHAR's width. Needed for APIs that hardcode
// std::string in their signature (e.g. Emulator_SaveImage/LoadImage)
// rather than using TCHAR/LPCTSTR.
std::string WStringToNarrowString(const std::wstring& ws)
{
    std::vector<char> buf(ws.size() * MB_CUR_MAX + 1);
    std::wcstombs(buf.data(), ws.c_str(), buf.size());
    return std::string(buf.data());
}

// Save full 64K memory dump to a file ("memdump.bin" by default).
// Ported from ConsoleView_SaveMemoryDump, using standard C++ file I/O
// instead of Win32 CreateFile/WriteFile.
bool SaveMemoryDump(const std::wstring& wfilename)
{
    std::wstring filename = wfilename.empty() ? L"memdump.bin" : wfilename;
    std::basic_string<TCHAR> tfilename = WStringToTString(filename);

    std::vector<uint8_t> buf(65536);
    for (int i = 0; i < 65536; i++)
        buf[i] = g_pBoard->GetByte((uint16_t)i, true);

    std::ofstream file(tfilename.c_str(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    return file.good();
}

// Load a .bin file (the classic BK tape image format: a 2-word header --
// little-endian start address, then little-endian byte count -- followed
// by that many bytes of raw memory image) and copy its data into emulated
// RAM at the given start address.
//
// Returns an empty string on success, or a human-readable error message on
// failure (file not found / unreadable, oversized, or header size mismatch).
std::wstring LoadBin(const std::wstring& wfilename)
{
    std::basic_string<TCHAR> tfilename = WStringToTString(wfilename);

    std::ifstream file(tfilename.c_str(), std::ios::binary);
    if (!file.is_open())
        return L"could not open file";

    // Read up to 4 (header) + 65536 (max RAM image) bytes.
    const size_t maxTotal = 4 + 65536;
    std::vector<uint8_t> buf(maxTotal);
    file.read(reinterpret_cast<char*>(buf.data()), maxTotal);
    std::streamsize bytesRead = file.gcount();

    if (bytesRead < 4)
        return L"file too small, missing header";

    uint16_t startAddress = (uint16_t)(buf[0] | (buf[1] << 8));
    uint16_t sizeField     = (uint16_t)(buf[2] | (buf[3] << 8));

    size_t dataBytes = (size_t)bytesRead - 4;
    if ((size_t)sizeField != dataBytes)
    {
        // Distinguish "there's more data in the file than we read" (file
        // larger than our 4+65536 cap) from a genuine header/size mismatch,
        // since the two call for different messages.
        if (dataBytes == 65536 && !file.eof())
            return L"file too large (more than 65536 bytes of data)";
        return L"size field in header does not match file size";
    }

    for (size_t i = 0; i < dataBytes; i++)
        g_pBoard->SetByte((uint16_t)(startAddress + i), true, buf[4 + i]);

    return L"";
}

// Render the current screen and save it as a PNG file.
// screenMode: 0 = black/white, 1 = color (see Emulator_PrepareScreenRGB32).
bool SaveScreenshot(const std::wstring& wfilename, int screenMode)
{
    int width = 0, height = 0;
    Emulator_GetScreenSize(screenMode, &width, &height);
    if (width <= 0 || height <= 0)
        return false;

    std::vector<uint32_t> bits((size_t)width * height);
    Emulator_PrepareScreenRGB32(bits.data(), screenMode);

    const uint32_t* palette = (screenMode == 0) ? ScreenView_BWPalette : ScreenView_ColorPalette;

    std::basic_string<TCHAR> filename = WStringToTString(wfilename);
    return PngFile_SaveScreenshot(bits.data(), palette, filename.c_str(), width, height);
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
    std::wstring paramFilename;
};

//////////////////////////////////////////////////////////////////////
// Console command handlers
// Mirrors the ConsoleView_Cmd* functions.

void CmdShowHelp(const ConsoleCommandParams& /*params*/)
{
    std::wcout <<
        L"Console command list:\n"
        L"  h, help, ?     Show this help\n"
        L"  reset          Reset the machine\n"
        L"  g, go          Go; free run\n"
        L"  gXXXXXX, go XXXXXX  Go; run and stop at address XXXXXX\n"
        L"  go frames N    Go; run for N frames, decimal (1 sec = 25 frames)\n"
        L"  s, step        Step Into; executes one instruction\n"
        L"  so, stepover   Step Over; executes and stops after the current instruction\n"
        L"  r, regs        Show register values\n"
        L"  regs ext       Show extended (I/O port) registers\n"
        L"  status         Show machine status: uptime, floppy drives\n"
        L"  rN             Show value of register N; N=0..7\n"
        L"  rN=XXXXXX      Set register N to value XXXXXX; N=0..7\n"
        L"  rps            Show PS (processor status word)\n"
        L"  rps=XXXXXX     Set PS to value XXXXXX\n"
        L"  rpc            Show PC (same as the PC shown by r/regs)\n"
        L"  rpc=XXXXXX     Set PC to value XXXXXX\n"
        L"  rsp            Show SP (same as the SP shown by r/regs)\n"
        L"  rsp=XXXXXX     Set SP to value XXXXXX\n"
        L"  d, disasm      Disassemble from PC; use D for short format\n"
        L"  dXXXXXX, disasm XXXXXX  Disassemble from address XXXXXX\n"
        L"  m, memory      Memory dump at current address\n"
        L"  mXXXXXX, memory XXXXXX  Memory dump at address XXXXXX\n"
        L"  memory bytes XXXXXX  Memory dump at address XXXXXX, byte granularity\n"
        L"  b              List all breakpoints\n"
        L"  bXXXXXX        Set breakpoint at address XXXXXX\n"
        L"  bc             Remove all breakpoints\n"
        L"  bcXXXXXX       Remove breakpoint at address XXXXXX\n"
        L"  mo, monitor    Type M O / Enter (BASIC) or P SPACE M / Enter (FOCAL) to exit to Monitor\n"
        L"  t, trace       Toggle instruction tracing to trace.log on/off\n"
        L"  tXXXXXX, trace XXXXXX  Set trace flags XXXXXX (see TRACE_xxx constants)\n"
        L"  tc             Clear trace.log\n"
        L"  memsave [FILE] Save memory dump as FILE; default memdump.bin\n"
        L"  loadbin FILE   Load a .bin file (tape image: start addr + size + data) into RAM\n"
        L"  statesave FILE Save full emulator state (memory, registers, ports) to FILE\n"
        L"  stateload FILE Load full emulator state from FILE\n"
        L"  diskN attach FILE  Attach floppy image FILE to drive N; N=A..D\n"
        L"  diskN detach   Detach floppy image from drive N; N=A..D\n"
        L"  screen [FILE]  Save black/white screenshot as FILE (PNG); default filename from timestamp\n"
        L"  screenc [FILE] Save color screenshot as FILE (PNG); default filename from timestamp\n"
        L"  q, quit, exit  Quit the debugger\n";
}

void CmdPrintAllRegisters(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    for (int r = 0; r < 8; r++)
    {
        TCHAR bufOctal[7];
        PrintOctalValue(bufOctal, pProc->GetReg(r));
        std::wcout << REGISTER_NAME[r] << L"=";
        Console_ColorModified(Emulator_IsRegisterChanged(r));
        std::wcout << bufOctal;
        Console_ColorReset();
        std::wcout << L" ";
    }
    TCHAR bufPSW[7];
    PrintOctalValue(bufPSW, pProc->GetPSW());
    uint16_t pswprev = g_wEmulatorPrevCpuR[8];
    uint16_t psw = g_wEmulatorCpuR[8];
    std::wcout << L"PSW=";
    Console_ColorModified(pswprev != psw);
    std::wcout << bufPSW;
    Console_ColorReset();
    std::wcout << L" [N=";
    Console_ColorModified((pswprev & PSW_N) != (psw & PSW_N));
    std::wcout << pProc->GetN();
    Console_ColorReset();
    std::wcout << L" Z=";
    Console_ColorModified((pswprev & PSW_Z) != (psw & PSW_Z));
    std::wcout << pProc->GetZ();
    Console_ColorReset();
    std::wcout << L" V=";
    Console_ColorModified((pswprev & PSW_V) != (psw & PSW_V));
    std::wcout << pProc->GetV();
    Console_ColorReset();
    std::wcout << L" C=";
    Console_ColorModified((pswprev & PSW_C) != (psw & PSW_C));
    std::wcout << pProc->GetC();
    Console_ColorReset();
    std::wcout << L" T=";
    Console_ColorModified((pswprev & PSW_T) != (psw & PSW_T));
    std::wcout << ((pProc->GetPSW() & PSW_T) != 0 ? 1 : 0);
    Console_ColorReset();
    std::wcout << L"]" << std::endl;
}

// Print one extended (I/O port) register line: address, value, label.
// Address is printed as given (some labels intentionally share a printed
// address while reading distinct underlying state via a different
// PORTVIEW_xxx selector -- e.g. "keyb data" and "palette" both show 177662).
void PrintPortRegisterLine(uint16_t printedAddress, uint16_t portViewSelector, LPCTSTR label)
{
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, printedAddress);
    TCHAR bufValue[7];
    PrintOctalValue(bufValue, g_pBoard->GetPortView(portViewSelector));
    std::wcout << bufAddr << L" " << bufValue << L" " << label << std::endl;
}

void CmdPrintExtendedRegisters(const ConsoleCommandParams& /*params*/)
{
    PrintPortRegisterLine(0177660, PORTVIEW_KEYBSTATUS,   _T("keyb state"));
    PrintPortRegisterLine(0177662, PORTVIEW_KEYBDATA,     _T("keyb data"));
    PrintPortRegisterLine(0177662, PORTVIEW_PALETTE,      _T("palette"));
    PrintPortRegisterLine(0177664, PORTVIEW_SCROLL,       _T("scroll"));
    PrintPortRegisterLine(0177706, PORTVIEW_TIMERREL,     _T("timer rel"));
    PrintPortRegisterLine(0177710, PORTVIEW_TIMERVAL,     _T("timer val"));
    PrintPortRegisterLine(0177712, PORTVIEW_TIMERCTL,     _T("timer ctl"));
    PrintPortRegisterLine(0177714, PORTVIEW_PARALLELIN,   _T("parallel in"));
    PrintPortRegisterLine(0177714, PORTVIEW_PARALLELOUT,  _T("parallel out"));
    PrintPortRegisterLine(0177716, PORTVIEW_SYSTEM,       _T("system"));
    PrintPortRegisterLine(0177716, PORTVIEW_SYSTEMMEM,    _T("system mem"));
    PrintPortRegisterLine(0177716, PORTVIEW_SYSTEMTAP,    _T("system tape"));

    if ((g_nEmulatorConfiguration & BK_COPT_FDD) != 0)
    {
        PrintPortRegisterLine(0177130, PORTVIEW_FDDSTATE, _T("floppy state"));
        PrintPortRegisterLine(0177132, PORTVIEW_FDDDATA,  _T("floppy data"));
    }
}

void CmdShowStatus(const ConsoleCommandParams& /*params*/)
{
    float uptime = Emulator_GetUptime();
    std::wcout << L"Uptime: " << std::fixed << std::setprecision(2) << uptime << L" sec" << std::endl;

    if ((g_nEmulatorConfiguration & BK_COPT_FDD) != 0)
    {
        std::wcout << L"Floppy engine: " << (Emulator_IsFloppyEngineOn() ? L"ON" : L"off") << std::endl;
        for (int slot = 0; slot < 4; slot++)
        {
            wchar_t letter = (wchar_t)(L'A' + slot);
            bool okAttached = Emulator_IsFloppyImageAttached(slot);
            std::wcout << L"  disk" << letter << L": ";
            if (okAttached)
            {
                std::wcout << L"attached"
                            << (Emulator_IsFloppyReadOnly(slot) ? L", read-only" : L", read-write");
            }
            else
            {
                std::wcout << L"not attached";
            }
            std::wcout << std::endl;
        }
    }
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

void CmdPrintRegisterSP(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t value = pProc->GetReg(6);
    PrintRegisterLine(_T("SP"), value);
}

void CmdSetRegisterSP(const ConsoleCommandParams& params)
{
    uint16_t value = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    pProc->SetReg(6, value);
}

void CmdPrintRegisterPC(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t value = pProc->GetReg(7);
    PrintRegisterLine(_T("PC"), value);
}

void CmdSetRegisterPC(const ConsoleCommandParams& params)
{
    uint16_t value = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    pProc->SetReg(7, value);
}

void CmdReset(const ConsoleCommandParams& /*params*/)
{
    Emulator_Reset();
}

void CmdStepInto(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    PrintDisassemble(pProc, pProc->GetPC(), true, false);
    g_pBoard->DebugTicks();
    Emulator_OnUpdate();  // Refresh change-tracking snapshot after this single instruction
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
        Emulator_OnUpdate();  // Refresh change-tracking snapshot after this single instruction
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

void CmdSaveMemoryDump(const ConsoleCommandParams& params)
{
    std::wstring filename = params.paramFilename.empty() ? L"memdump.bin" : params.paramFilename;
    if (SaveMemoryDump(filename))
        std::wcout << L"Saved memory dump " << filename << std::endl;
    else
        std::wcout << L"FAILED to save memory dump " << filename << std::endl;
}

void CmdLoadBin(const ConsoleCommandParams& params)
{
    std::wstring error = LoadBin(params.paramFilename);
    if (error.empty())
        std::wcout << L"Loaded " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to load " << params.paramFilename << L": " << error << std::endl;
}

void CmdStateSave(const ConsoleCommandParams& params)
{
    std::string filename = WStringToNarrowString(params.paramFilename);
    if (Emulator_SaveImage(filename))
        std::wcout << L"Saved state " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to save state " << params.paramFilename << std::endl;
}

void CmdStateLoad(const ConsoleCommandParams& params)
{
    std::string filename = WStringToNarrowString(params.paramFilename);
    if (Emulator_LoadImage(filename))
        std::wcout << L"Loaded state " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to load state " << params.paramFilename << std::endl;
}

// "diskA attach FILE" .. "diskD attach FILE" -- the slot letter is the 5th
// character of commandText ("disk" is 4 chars), A=slot 0 .. D=slot 3.
void CmdAttachFloppyImage(const ConsoleCommandParams& params)
{
    wchar_t letter = params.commandText[4];
    int slot = letter - L'A';

    std::basic_string<TCHAR> tfilename = WStringToTString(params.paramFilename);
    bool result = Emulator_AttachFloppyImage(slot, tfilename.c_str());

    if (result)
        std::wcout << L"Attached disk" << letter << L": " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to attach disk" << letter << L": " << params.paramFilename << std::endl;
}

// "diskA detach" .. "diskD detach" -- same slot-letter convention as attach.
void CmdDetachFloppyImage(const ConsoleCommandParams& params)
{
    wchar_t letter = params.commandText[4];
    int slot = letter - L'A';

    Emulator_DetachFloppyImage(slot);
    std::wcout << L"Detached disk" << letter << std::endl;
}

void CmdScreenshot(const ConsoleCommandParams& params)
{
    bool okColor = (params.commandText.compare(0, 7, L"screenc") == 0);
    int screenMode = okColor ? 1 : 0;

    std::wstring filename = params.paramFilename;
    if (filename.empty())
    {
        // Generate default filename from current local time: YYYYMMDDHHMMSSmmm.png
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        std::tm tm = *std::localtime(&t);
        wchar_t buf[32];
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
                      L"%04d%02d%02d%02d%02d%02d%03d.png",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      (int)ms.count());
        filename = buf;
    }

    bool result = SaveScreenshot(filename, screenMode);

    if (result)
        std::wcout << L"Saved screenshot " << filename << std::endl;
    else
        std::wcout << L"FAILED to save screenshot " << filename << std::endl;
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

void CmdPrintMemoryDumpBytesAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    PrintMemoryDumpBytes(pProc, address);
}

// Run until a breakpoint is hit, or until maxFrames frames have been run.
// Since this console has no message loop / timer driving frames the way the
// GUI build does, "go" here just drives Emulator_SystemFrame() in a blocking
// loop until it reports a breakpoint.
//
// Safety valve: there is no way to interrupt this from another thread (no
// GUI Stop button, no Ctrl+C handler), so a free run with no breakpoint
// ever set -- or a breakpoint at an address the CPU never reaches -- would
// hang the console forever. maxFrames caps it; at 25 frames/sec the default
// 3000 is ~2 minutes of emulated time before giving up.
void RunUntilBreakpoint(int maxFrames)
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
        if (frames >= maxFrames)
        {
            Emulator_Stop();
            break;
        }
    }
    Emulator_OnUpdate();  // Refresh change-tracking snapshot now that we've stopped
    CProcessor* pProc = GetCurrentProcessor();
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, pProc->GetPC());
    if (hitBreakpoint)
        std::wcout << L" Stopped at " << bufAddr << std::endl;
    else
        std::wcout << L" Stopped at " << bufAddr << L" (no breakpoint hit after "
                    << maxFrames << L" frames)" << std::endl;
}

void CmdRun(const ConsoleCommandParams& /*params*/)
{
    RunUntilBreakpoint();
}

void CmdRunFrames(const ConsoleCommandParams& params)
{
    RunUntilBreakpoint((int)params.paramOct1);
}

void CmdRunToAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    Emulator_SetTempCPUBreakpoint(address);
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
const uint8_t BK_KEY_ENTER = 0015;  // CR / Enter

void GotoMonitor()
{
    if ((g_nEmulatorConfiguration & BK_COPT_ROM_BASIC) != 0)
    {
        TypeKey(BK_KEY_M);
        TypeKey(BK_KEY_O);
        TypeKey(BK_KEY_ENTER);
    }
    else if ((g_nEmulatorConfiguration & BK_COPT_ROM_FOCAL) != 0)
    {
        TypeKey(BK_KEY_P);
        TypeKey(BK_KEY_SPACE);
        TypeKey(BK_KEY_M);
        TypeKey(BK_KEY_ENTER);
    }
}

void CmdGotoMonitor(const ConsoleCommandParams& /*params*/)
{
    GotoMonitor();
}

// Set the board's trace mask and report the new state.
// Mirrors ConsoleView_TraceLog: turning tracing off also closes the log
// file handle so trace.log is flushed and available immediately.
void TraceLog(uint32_t value)
{
    g_pBoard->SetTrace(value);
    if (value != TRACE_NONE)
    {
        TCHAR bufFlags[7];
        PrintOctalValue(bufFlags, (uint16_t)g_pBoard->GetTrace());
        std::wcout << L" Trace ON, trace flags " << bufFlags << std::endl;
    }
    else
    {
        std::wcout << L" Trace OFF." << std::endl;
        DebugLogCloseFile();
    }
}

void CmdTraceLogWithMask(const ConsoleCommandParams& params)
{
    TraceLog(params.paramOct1);
}

void CmdTraceLogOnOff(const ConsoleCommandParams& /*params*/)
{
    uint32_t dwTrace = (g_pBoard->GetTrace() == TRACE_NONE ? TRACE_ALL : TRACE_NONE);
    TraceLog(dwTrace);
}

void CmdClearTraceLog(const ConsoleCommandParams& /*params*/)
{
    DebugLogClear();
    std::wcout << L" Trace log cleared." << std::endl;
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
    ARGINFO_DEC,        // Decimal value
    ARGINFO_REG_OCT,    // Register number, octal value
    ARGINFO_FILENAME,       // A space then a filename (rest of the line, verbatim)
    ARGINFO_OPT_FILENAME,   // Optional: either bare command, or space + filename
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
    { L"?",     ARGINFO_NONE,    CmdShowHelp },
    { L"help",  ARGINFO_NONE,    CmdShowHelp },
    { L"h",     ARGINFO_NONE,    CmdShowHelp },

    { L"r",     ARGINFO_REG_OCT, CmdSetRegisterValue },          // rN=XXXXXX
    { L"r",     ARGINFO_REG,     CmdPrintRegister },              // rN
    { L"rps=",  ARGINFO_OCT,     CmdSetRegisterPSW },             // rps=XXXXXX
    { L"rps ",  ARGINFO_OCT,     CmdSetRegisterPSW },             // rps XXXXXX
    { L"rps",   ARGINFO_NONE,    CmdPrintRegisterPSW },           // rps
    { L"rpc=",  ARGINFO_OCT,     CmdSetRegisterPC },              // rpc=XXXXXX
    { L"rpc ",  ARGINFO_OCT,     CmdSetRegisterPC },              // rpc XXXXXX
    { L"rpc",   ARGINFO_NONE,    CmdPrintRegisterPC },            // rpc
    { L"rsp=",  ARGINFO_OCT,     CmdSetRegisterSP },              // rsp=XXXXXX
    { L"rsp ",  ARGINFO_OCT,     CmdSetRegisterSP },              // rsp XXXXXX
    { L"rsp",   ARGINFO_NONE,    CmdPrintRegisterSP },            // rsp
    { L"regs ext", ARGINFO_NONE,  CmdPrintExtendedRegisters },     // regs ext
    { L"status",   ARGINFO_NONE,  CmdShowStatus },                 // status
    { L"regs",  ARGINFO_NONE,    CmdPrintAllRegisters },          // regs
    { L"r",     ARGINFO_NONE,    CmdPrintAllRegisters },          // r

    { L"stepover", ARGINFO_NONE,    CmdStepOver },
    { L"so",      ARGINFO_NONE,    CmdStepOver },
    { L"step",    ARGINFO_NONE,    CmdStepInto },
    { L"s",       ARGINFO_NONE,    CmdStepInto },

    { L"reset", ARGINFO_NONE,    CmdReset },

    { L"disasm ", ARGINFO_OCT,   CmdPrintDisassembleAtAddress }, // disasm XXXXXX
    { L"disasm",  ARGINFO_NONE,  CmdPrintDisassembleAtPC },      // disasm
    { L"d",     ARGINFO_OCT,     CmdPrintDisassembleAtAddress },  // dXXXXXX
    { L"D",     ARGINFO_OCT,     CmdPrintDisassembleAtAddress },  // DXXXXXX
    { L"d",     ARGINFO_NONE,    CmdPrintDisassembleAtPC },       // d
    { L"D",     ARGINFO_NONE,    CmdPrintDisassembleAtPC },       // D

    { L"memsave", ARGINFO_OPT_FILENAME, CmdSaveMemoryDump },        // memsave [FILE]
    { L"loadbin", ARGINFO_FILENAME,     CmdLoadBin },                // loadbin FILENAME
    { L"statesave", ARGINFO_FILENAME,   CmdStateSave },              // statesave FILENAME
    { L"stateload", ARGINFO_FILENAME,   CmdStateLoad },              // stateload FILENAME

    { L"diskA attach", ARGINFO_FILENAME, CmdAttachFloppyImage },     // diskA attach FILENAME
    { L"diskB attach", ARGINFO_FILENAME, CmdAttachFloppyImage },     // diskB attach FILENAME
    { L"diskC attach", ARGINFO_FILENAME, CmdAttachFloppyImage },     // diskC attach FILENAME
    { L"diskD attach", ARGINFO_FILENAME, CmdAttachFloppyImage },     // diskD attach FILENAME
    { L"diskA detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // diskA detach
    { L"diskB detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // diskB detach
    { L"diskC detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // diskC detach
    { L"diskD detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // diskD detach

    { L"screenc", ARGINFO_OPT_FILENAME, CmdScreenshot },
    { L"screen",  ARGINFO_OPT_FILENAME, CmdScreenshot },

    { L"monitor", ARGINFO_NONE,   CmdGotoMonitor },
    { L"mo",      ARGINFO_NONE,   CmdGotoMonitor },

    { L"tc",    ARGINFO_NONE,    CmdClearTraceLog },              // tc
    { L"trace ", ARGINFO_OCT,    CmdTraceLogWithMask },           // trace XXXXXX
    { L"t",     ARGINFO_OCT,     CmdTraceLogWithMask },           // tXXXXXX
    { L"trace", ARGINFO_NONE,    CmdTraceLogOnOff },              // trace
    { L"t",     ARGINFO_NONE,    CmdTraceLogOnOff },              // t
    { L"memory bytes ", ARGINFO_OCT, CmdPrintMemoryDumpBytesAtAddress }, // memory bytes XXXXXX
    { L"memory ", ARGINFO_OCT,   CmdPrintMemoryDumpAtAddress },  // memory XXXXXX
    { L"memory",  ARGINFO_NONE,   CmdPrintMemoryDumpAtPC },       // memory
    { L"m",       ARGINFO_OCT,    CmdPrintMemoryDumpAtAddress },  // mXXXXXX
    { L"m",       ARGINFO_NONE,   CmdPrintMemoryDumpAtPC },       // m

    { L"go frames ", ARGINFO_DEC,  CmdRunFrames },                  // go frames N (decimal)
    { L"go ",   ARGINFO_OCT,     CmdRunToAddress },               // go XXXXXX
    { L"go",    ARGINFO_NONE,    CmdRun },                        // go
    { L"g",     ARGINFO_OCT,     CmdRunToAddress },               // gXXXXXX
    { L"g",     ARGINFO_NONE,    CmdRun },                        // g

    { L"bc",    ARGINFO_OCT,     CmdRemoveBreakpointAtAddress },  // bcXXXXXX
    { L"bc",    ARGINFO_NONE,    CmdRemoveAllBreakpoints },       // bc
    { L"b",     ARGINFO_OCT,     CmdSetBreakpointAtAddress },     // bXXXXXX
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

    case ARGINFO_DEC:
        {
            if (rest.empty())
                return false;
            for (wchar_t ch : rest)
                if (ch < L'0' || ch > L'9') return false;
            uint32_t value = 0;
            for (wchar_t ch : rest)
            {
                value = value * 10 + (uint32_t)(ch - L'0');
                if (value > 0xffff) value = 0xffff;  // clamp, paramOct1 is 16-bit
            }
            params.paramOct1 = (uint16_t)value;
            return true;
        }

    case ARGINFO_REG_OCT:
        {
            // Accept "N=XXXXXX" or "N XXXXXX"
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

    case ARGINFO_FILENAME:
        {
            // Expect a single space then a non-empty filename, e.g. "screen shot.png"
            if (rest.empty() || rest[0] != L' ')
                return false;
            std::wstring filename = rest.substr(1);
            if (filename.empty())
                return false;
            params.paramFilename = filename;
            return true;
        }

    case ARGINFO_OPT_FILENAME:
        {
            // Bare command (no filename) or "command FILENAME"
            if (rest.empty())
                return true;  // paramFilename stays empty; caller generates default name
            if (rest[0] != L' ')
                return false;
            params.paramFilename = rest.substr(1);
            return !params.paramFilename.empty();
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////

void PrintConsolePrompt()
{
    CProcessor* pProc = GetCurrentProcessor();
    TCHAR bufAddr[7];
    Console_ColorPrompt();
    PrintOctalValue(bufAddr, pProc->GetPC());
    std::wcout << bufAddr << L"> ";
    Console_ColorReset();
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
