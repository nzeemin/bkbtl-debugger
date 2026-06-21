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
#include <type_traits>
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

// Modifier postfixes for "examine"/"x": any combination, any order, e.g.
// "examine bytes hex", "x100260 hex nochars", "x bytes".
const uint32_t EXAMFLAG_BYTES    = 0x01;  // Byte granularity instead of word
const uint32_t EXAMFLAG_HEX      = 0x02;  // Hexadecimal instead of octal
const uint32_t EXAMFLAG_NOCHARS  = 0x04;  // Hide the trailing ASCII/character column

//////////////////////////////////////////////////////////////////////
// Continuation ("paging") for "examine"/"x" and "disasm"/"d"/"D".
//
// After printing a page, these commands can leave a "continuation" armed:
// the next address to show plus the modifiers/format that produced the
// current page. The main loop (bkbtldebug.cpp) checks for this before
// showing the normal prompt; if armed, it shows "-- more --" instead and
// reads a line. An empty line (just Enter) re-runs the same paging
// command at the saved address. Any other input is just "stop paging" --
// it answers the prompt, it is not a command line, so it's discarded and
// control returns to the normal prompt.

enum class ContinuationKind { None, Examine, Disasm };

struct ContinuationState
{
    ContinuationKind kind = ContinuationKind::None;
    uint16_t address = 0;
    uint32_t examineFlags = 0;  // Used when kind == Examine
    bool disasmShort = false;   // Used when kind == Disasm (D vs d)
};

ContinuationState g_continuation;

// Print the "-- more --" prompt in the same color as the regular command
// prompt, and arm the given continuation so the next blank Enter resumes.
void ArmContinuation(const ContinuationState& state)
{
    g_continuation = state;
    Console_ColorPrompt();
    std::wcout << L"-- more (Enter to continue) --";
    Console_ColorReset();
}

CProcessor* GetCurrentProcessor()
{
    return g_pBoard->GetCPU();
}

// Forward declaration: RunUntilBreakpoint is defined further down (near the
// "continue" commands) but is also used by CmdStepOver defined before it.
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
int PrintDisassemble(CProcessor* pProc, uint16_t address, bool okOneInstr, bool okShort, uint16_t* pNextAddress = nullptr)
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
    if (pNextAddress != nullptr)
        *pNextAddress = address;
    return lastLength;
}

// Print a memory dump: address, then either 8 words or 16 bytes (per
// EXAMFLAG_BYTES), in octal or hex (per EXAMFLAG_HEX), then optionally
// their ASCII/character representation (suppressed by EXAMFLAG_NOCHARS).
// Always covers 16 bytes (one "line") per row regardless of granularity,
// so word and byte dumps of the same region line up the same way.
void PrintMemoryDumpGeneric(const CProcessor* pProc, uint16_t address, uint32_t flags, int lines = 8, uint16_t* pNextAddress = nullptr)
{
    bool okBytes = (flags & EXAMFLAG_BYTES) != 0;
    bool okHex = (flags & EXAMFLAG_HEX) != 0;
    bool okChars = (flags & EXAMFLAG_NOCHARS) == 0;

    if (!okBytes)
        address &= ~1;  // Word dumps line up to an even address
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
        if (okHex)
            PrintHexValue(bufAddr, address);
        else
            PrintOctalValue(bufAddr, address);
        std::wcout << L"  " << bufAddr << L"  ";

        for (int i = 0; i < 8; i++)
        {
            uint16_t word = dump[i];
            if (changed[i] != 0) Console_ColorModified();
            if (okBytes)
            {
                TCHAR bufValue[7];
                if (okHex)
                {
                    PrintHexValue(bufValue, word & 0xFF);
                    std::wcout << (bufValue + 2) << L" ";
                    PrintHexValue(bufValue, word >> 8);
                    std::wcout << (bufValue + 2) << L" ";
                }
                else
                {
                    PrintOctalValue(bufValue, word & 0xFF);
                    std::wcout << (bufValue + 3) << L" ";
                    PrintOctalValue(bufValue, word >> 8);
                    std::wcout << (bufValue + 3) << L" ";
                }
            }
            else
            {
                TCHAR bufValue[7];
                if (okHex)
                    PrintHexValue(bufValue, word);
                else
                    PrintOctalValue(bufValue, word);
                std::wcout << bufValue << L" ";
            }
            Console_ColorReset();
        }

        if (okChars)
        {
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
        }
        std::wcout << std::endl;

        address += 16;
    }
    if (pNextAddress != nullptr)
        *pNextAddress = address;
}

// Convert a wstring command-line argument (e.g. a filename) to a TCHAR
// string. Under GCC, TCHAR is plain char, so this narrows via the current
// C locale; under real MSVC, TCHAR is wchar_t, so it's effectively a copy.
//
// Templated on a dummy parameter so only the branch matching TCHAR's
// actual type needs to type-check: plain "if constexpr" inside a
// non-template function still requires both branches to be well-formed
// even though only one runs, and basic_string<char> cannot convert to
// basic_string<wchar_t> (or vice versa) as a return statement regardless
// of which branch executes. (Checking #ifdef _UNICODE here, as an earlier
// version of this function did, is wrong: this project never defines
// _UNICODE anywhere, on any platform, so that branch was always dead and
// the function always silently took the narrow path -- harmless under
// GCC where TCHAR is already char, but it meant this function has never
// actually been exercised, or correct, under a genuine TCHAR=wchar_t
// build until this fix.)
template <typename T>
std::basic_string<T> WStringToTStringImpl(const std::wstring& ws)
{
    if constexpr (std::is_same_v<T, wchar_t>)
    {
        return ws;
    }
    else
    {
        return WStringToNarrowString(ws);
    }
}

std::basic_string<TCHAR> WStringToTString(const std::wstring& ws)
{
    return WStringToTStringImpl<TCHAR>(ws);
}

// Save full 64K memory dump to a file ("memdump.bin" by default).
// Ported from ConsoleView_SaveMemoryDump, using standard C++ file I/O
// instead of Win32 CreateFile/WriteFile.
bool SaveMemoryDump(const std::wstring& wfilename)
{
    std::wstring filename = wfilename.empty() ? L"memdump.bin" : wfilename;
    std::string narrowFilename = WStringToNarrowString(filename);

    std::vector<uint8_t> buf(65536);
    for (int i = 0; i < 65536; i++)
        buf[i] = g_pBoard->GetByte((uint16_t)i, true);

    std::ofstream file(narrowFilename.c_str(), std::ios::binary | std::ios::trunc);
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
    std::string narrowFilename = WStringToNarrowString(wfilename);

    std::ifstream file(narrowFilename.c_str(), std::ios::binary);
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
    uint32_t paramFlags = 0;     // Modifier postfix flags, see EXAMFLAG_xxx
    bool paramHasAddress = false; // Whether an explicit address was given (vs PC default)
    size_t paramPrefixLength = 0; // Length of the table prefix that matched commandText
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
        L"  c, continue    Continue; free run\n"
        L"  cXXXXXX, continue XXXXXX  Continue; run and stop at address XXXXXX\n"
        L"  continue frames N  Continue; run for N frames, decimal (1 sec = 25 frames)\n"
        L"  s, step        Step Into; executes one instruction\n"
        L"  n, next        Step Over (Next); executes and stops after the current instruction\n"
        L"  r, regs        Show register values\n"
        L"  r ext, regs ext  Show extended (I/O port) registers\n"
        L"  i, info        Show machine status: uptime, floppy drives\n"
        L"  i floppy, info floppy  Show floppy controller registers and state\n"
        L"  rN             Show value of register N; N=0..7\n"
        L"  rN=XXXXXX      Set register N to value XXXXXX; N=0..7\n"
        L"  rps            Show PS (processor status word)\n"
        L"  rps=XXXXXX     Set PS to value XXXXXX\n"
        L"  rpc            Show PC (same as the PC shown by r/regs)\n"
        L"  rpc=XXXXXX     Set PC to value XXXXXX\n"
        L"  rsp            Show SP (same as the SP shown by r/regs)\n"
        L"  rsp=XXXXXX     Set SP to value XXXXXX\n"
        L"  d, disasm      Disassemble from PC; use D for short format; paged output\n"
        L"  dXXXXXX, disasm XXXXXX  Disassemble from address XXXXXX\n"
        L"  x, examine     Examine memory at current address; paged output\n"
        L"  xXXXXX, examine XXXXXX  Examine memory at address XXXXXX\n"
        L"  ... bytes      Modifier: byte granularity instead of words\n"
        L"  ... hex        Modifier: hexadecimal instead of octal\n"
        L"  ... nochars    Modifier: hide the ASCII/character column\n"
        L"                 Modifiers combine in any order, e.g. \"x100260 bytes hex\"\n"
        L"  b              List all breakpoints\n"
        L"  bXXXXXX        Set breakpoint at address XXXXXX\n"
        L"  bc             Remove all breakpoints\n"
        L"  bcXXXXXX       Remove breakpoint at address XXXXXX\n"
        L"  t, trace       Toggle instruction tracing to trace.log on/off\n"
        L"  tXXXXXX, trace XXXXXX  Set trace flags XXXXXX (see TRACE_xxx constants)\n"
        L"  tc, t clear, trace clear  Clear trace.log\n"
        L"  memsave [FILE] Save memory dump as FILE; default memdump.bin\n"
        L"  loadbin FILE   Load a .bin file (tape image: start addr + size + data) into RAM\n"
        L"  statesave FILE Save full emulator state (memory, registers, ports) to FILE\n"
        L"  stateload FILE Load full emulator state from FILE\n"
        L"  diskN attach FILE, diskN a FILE  Attach floppy image FILE to drive N; N=A..D\n"
        L"  diskN detach, diskN d  Detach floppy image from drive N; N=A..D\n"
        L"  screen [FILE]  Save black/white screenshot as FILE (PNG); default filename from timestamp\n"
        L"  screenc [FILE] Save color screenshot as FILE (PNG); default filename from timestamp\n"
        L"  kd KEY, key down KEY    Press and hold KEY\n"
        L"  ku KEY, key up KEY      Release KEY\n"
        L"  k KEY, key KEY          Click KEY: press, wait, release\n"
        L"  k MOD+KEY, key MOD+KEY  Hold MOD, click KEY, release MOD\n"
        L"                 KEY/MOD = letter, digit, punctuation, named key, or octal scancode\n"
        L"                 Named keys: ENTER SPACE TAB BACKSPACE LEFT RIGHT UP DOWN RUS LAT VS\n"
        L"                 REPEAT LOWER UPPER STOP AR2 SHIFT SU (AR2/SHIFT/SU are modifiers)\n"
        L"  mo, monitor    Type M O / Enter (BASIC) or P SPACE M / Enter (FOCAL) to exit to Monitor\n"
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

// Print "Floppy engine: ON/off" and the per-drive attach/read-only list.
// Caller must have already checked BK_COPT_FDD.
// okMarkSelected: append "(selected)" to the currently selected drive's
// line (used by "info floppy"; "i"/"info" doesn't show this).
void PrintFloppyEngineAndDrives(bool okMarkSelected)
{
    std::wcout << L"Floppy engine: " << (Emulator_IsFloppyEngineOn() ? L"ON" : L"off") << std::endl;

    int selectedDrive = -1;
    if (okMarkSelected)
    {
        uint16_t driveValue = g_pBoard->GetPortView(PORTVIEW_FDDDRIVE);
        if (driveValue != 0xFFFF)
            selectedDrive = driveValue;
    }

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
        if (slot == selectedDrive)
            std::wcout << L" (selected)";
        std::wcout << std::endl;
    }
}

void CmdPrintFloppyRegisters(const ConsoleCommandParams& /*params*/)
{
    if ((g_nEmulatorConfiguration & BK_COPT_FDD) == 0)
    {
        std::wcout << L" Current configuration has no floppy controller." << std::endl;
        return;
    }

    PrintFloppyEngineAndDrives(true);

    PrintPortRegisterLine(0177130, PORTVIEW_FDDSTATE, _T("floppy state"));
    PrintPortRegisterLine(0177132, PORTVIEW_FDDDATA,  _T("floppy data"));

    // Track/side are internal controller state, not memory-mapped ports --
    // there's no real address for them -- but GetPortView() is still the
    // debugger access path used for everything else above, so they go
    // through it too. Printed without an address column to avoid implying
    // they live at some address.
    TCHAR bufTrack[7];
    PrintOctalValue(bufTrack, g_pBoard->GetPortView(PORTVIEW_FDDTRACK));
    std::wcout << L"       " << bufTrack << L" track" << std::endl;

    TCHAR bufSide[7];
    PrintOctalValue(bufSide, g_pBoard->GetPortView(PORTVIEW_FDDSIDE));
    std::wcout << L"       " << bufSide << L" side" << std::endl;
}

void CmdShowStatus(const ConsoleCommandParams& /*params*/)
{
    float uptime = Emulator_GetUptime();
    std::wcout << L"Uptime: " << std::fixed << std::setprecision(2) << uptime << L" sec" << std::endl;

    if ((g_nEmulatorConfiguration & BK_COPT_FDD) != 0)
    {
        PrintFloppyEngineAndDrives(false);
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

    TCHAR bufValue[7];
    PrintOctalValue(bufValue, value);
    std::wcout << REGISTER_NAME[r] << L" set to " << bufValue << std::endl;
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

    TCHAR bufValue[7];
    PrintOctalValue(bufValue, value);
    std::wcout << L"PS set to " << bufValue << std::endl;
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

    TCHAR bufValue[7];
    PrintOctalValue(bufValue, value);
    std::wcout << L"SP set to " << bufValue << std::endl;
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

    TCHAR bufValue[7];
    PrintOctalValue(bufValue, value);
    std::wcout << L"PC set to " << bufValue << std::endl;
}

void CmdReset(const ConsoleCommandParams& /*params*/)
{
    Emulator_Reset();
    std::wcout << L"Reset." << std::endl;
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
    uint16_t nextAddress;
    PrintDisassemble(pProc, address, false, okShort, &nextAddress);

    ContinuationState state;
    state.kind = ContinuationKind::Disasm;
    state.address = nextAddress;
    state.disasmShort = okShort;
    ArmContinuation(state);
}

void CmdPrintDisassembleAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    bool okShort = (params.commandText[0] == L'D');
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t nextAddress;
    PrintDisassemble(pProc, address, false, okShort, &nextAddress);

    ContinuationState state;
    state.kind = ContinuationKind::Disasm;
    state.address = nextAddress;
    state.disasmShort = okShort;
    ArmContinuation(state);
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

// "examine"/"x": optional address (default PC), then any combination of
// postfix modifiers ("bytes", "hex", "nochars") in any order, e.g.
// "x", "x100260", "examine 100260 bytes hex", "x hex nochars".
void CmdExamineMemory(const ConsoleCommandParams& params)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t address = params.paramHasAddress ? params.paramOct1 : pProc->GetPC();
    uint16_t nextAddress;
    PrintMemoryDumpGeneric(pProc, address, params.paramFlags, 8, &nextAddress);

    ContinuationState state;
    state.kind = ContinuationKind::Examine;
    state.address = nextAddress;
    state.examineFlags = params.paramFlags;
    ArmContinuation(state);
}

// Run until a breakpoint is hit, or until maxFrames frames have been run.
// Since this console has no message loop / timer driving frames the way the
// GUI build does, "continue" here just drives Emulator_SystemFrame() in a
// blocking loop until it reports a breakpoint.
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

//////////////////////////////////////////////////////////////////////
// "key down KEY" / "kd KEY", "key up KEY" / "ku KEY", "key KEY" / "k KEY",
// "key MOD+KEY" / "k MOD+KEY" -- inject individual keyboard events.
//
// Scancodes are from BKBTL's emulator/KeyboardView.cpp (m_arrKeyboardKeys1 /
// m_arrKeyboardKeys2 -- same scancodes for both BK-0010 and BK-0011M, only
// the GUI layout differs between the two tables). Letters/digits/punctuation
// use the BK key that carries that unshifted character; e.g. "key :" and
// "key 0072" are the same key, and so is "key ;" via 0073 -- they are two
// different physical keys, not shifted variants of one another, because
// that's how the real BK keyboard matrix is laid out.

struct NamedKey
{
    const wchar_t* name;
    uint8_t scancode;
};

const NamedKey g_namedKeys[] =
{
    // Letters (BK scancode == ASCII code of the Latin letter on that key)
    { L"A", 0101 }, { L"B", 0102 }, { L"C", 0103 }, { L"D", 0104 },
    { L"E", 0105 }, { L"F", 0106 }, { L"G", 0107 }, { L"H", 0110 },
    { L"I", 0111 }, { L"J", 0112 }, { L"K", 0113 }, { L"L", 0114 },
    { L"M", 0115 }, { L"N", 0116 }, { L"O", 0117 }, { L"P", 0120 },
    { L"Q", 0121 }, { L"R", 0122 }, { L"S", 0123 }, { L"T", 0124 },
    { L"U", 0125 }, { L"V", 0126 }, { L"W", 0127 }, { L"X", 0130 },
    { L"Y", 0131 }, { L"Z", 0132 },
    // Digits
    { L"0", 0060 }, { L"1", 0061 }, { L"2", 0062 }, { L"3", 0063 },
    { L"4", 0064 }, { L"5", 0065 }, { L"6", 0066 }, { L"7", 0067 },
    { L"8", 0070 }, { L"9", 0071 },
    // Punctuation, named by the unshifted character on that key
    { L";",  0073 }, { L"-",  0055 }, { L"/",  0057 }, { L":",  0072 },
    { L",",  0054 }, { L".",  0056 }, { L"\\", 0134 }, { L"[",  0133 },
    { L"]",  0135 },
    // Named special keys
    { L"ENTER",     0012 },
    { L"SPACE",     0040 },
    { L"TAB",       0015 },
    { L"BACKSPACE", 0030 },
    { L"LEFT",      0010 },
    { L"RIGHT",     0031 },
    { L"UP",        0032 },
    { L"DOWN",      0033 },
    { L"RUS",       0016 },
    { L"LAT",       0017 },
    { L"VS",        0023 },  // ВС -- line feed / "СТРОКА ВВЕРХ"
    { L"REPEAT",    BK_KEY_REPEAT },
    { L"LOWER",     BK_KEY_LOWER },     // СТР -- lowercase lock
    { L"UPPER",     BK_KEY_UPPER },     // ЗАГЛ -- uppercase lock
    { L"STOP",      BK_KEY_STOP },
    // Modifiers -- only take effect while held; see "key MOD+KEY" below
    { L"AR2",       BK_KEY_AR2 },       // additional register / АР2
    { L"SHIFT",     BK_KEY_BACKSHIFT }, // small arrow down / lowercase-while-held
    { L"SU",        0000 },             // СУ -- control-code mode while held
};
const size_t g_namedKeysCount = sizeof(g_namedKeys) / sizeof(g_namedKeys[0]);

// How long a plain (non-modifier) key event is held visible to the
// emulator, in frames, before the matching press/release counterpart.
const int KEY_HOLD_FRAMES = 3;
// Same, but for a modifier held around another key in "key MOD+KEY".
const int KEY_MODIFIER_HOLD_FRAMES = 1;

// Look up a key by name (case-insensitive) or by raw octal scancode
// (digits only, e.g. "0102"). Returns true and fills *pScancode on success.
bool FindNamedKey(const std::wstring& name, uint8_t* pScancode)
{
    bool okAllOctalDigits = !name.empty();
    for (wchar_t ch : name)
        if (ch < L'0' || ch > L'7') { okAllOctalDigits = false; break; }
    if (okAllOctalDigits)
    {
        uint16_t value = 0;
        for (wchar_t ch : name)
            value = (uint16_t)((value << 3) + (ch - L'0'));
        if (value > 0377)
            return false;
        *pScancode = (uint8_t)value;
        return true;
    }

    for (size_t i = 0; i < g_namedKeysCount; i++)
    {
        if (WStringEqualsIgnoreCase(name, g_namedKeys[i].name))
        {
            *pScancode = g_namedKeys[i].scancode;
            return true;
        }
    }
    return false;
}

// Press (or release) one key and pump enough frames for the emulator to
// see it, the same way mo/TypeKey above does for a press+release pair.
void SetKeyState(uint8_t scancode, bool okPressed, bool okAr2, int holdFrames)
{
    Emulator_KeyEvent(scancode, okPressed, okAr2);
    PumpFrames(holdFrames);
}

// "key MOD+KEY": hold MOD, click KEY, release MOD, per the timing in the
// class comment above -- 1 frame around the modifier's own press/release,
// 3 frames around the target key's press/release (the final 3-frame wait
// after releasing MOD is deliberate: it's there so back-to-back "key ..."
// commands don't run together).
void ClickKeyWithModifier(uint8_t modScancode, uint8_t keyScancode)
{
    bool okAr2 = (modScancode == BK_KEY_AR2);
    SetKeyState(modScancode, true, okAr2, KEY_MODIFIER_HOLD_FRAMES);
    SetKeyState(keyScancode, true, okAr2, KEY_HOLD_FRAMES);
    SetKeyState(keyScancode, false, okAr2, KEY_MODIFIER_HOLD_FRAMES);
    SetKeyState(modScancode, false, false, KEY_HOLD_FRAMES);
}

// "key KEY" with no modifier: press, hold, release, hold.
void ClickKey(uint8_t scancode)
{
    SetKeyState(scancode, true, false, KEY_HOLD_FRAMES);
    SetKeyState(scancode, false, false, KEY_HOLD_FRAMES);
}

// Parse "KEY" or "MOD+KEY" out of params.commandText, given the prefix
// length the matched table row consumed (params.paramPrefixLength, set by
// MatchCommand -- this is the single source of truth for where the key
// spec starts, so CmdKeyDown/CmdKeyUp/CmdKeyClick don't each need their
// own logic to figure out whether "kd"/"ku"/"k" or "key down"/"key up"/
// "key" matched). paramPrefixLength covers only the literal table prefix
// (e.g. 2 for "kd", 8 for "key down"); the single space that ARGINFO_FILENAME
// requires right after it is not included, so it's skipped here. Reports
// an error and returns false if anything doesn't parse.
bool ParseKeySpec(const ConsoleCommandParams& params, size_t prefixLength,
                   uint8_t* pScancode, uint8_t* pModScancode, bool* pHasModifier)
{
    std::wstring spec = params.commandText.substr(prefixLength + 1);
    *pHasModifier = false;

    size_t plusPos = spec.find(L'+');
    if (plusPos != std::wstring::npos)
    {
        std::wstring modName = spec.substr(0, plusPos);
        std::wstring keyName = spec.substr(plusPos + 1);
        if (!FindNamedKey(modName, pModScancode))
        {
            std::wcout << L"Unknown modifier key: " << modName << std::endl;
            return false;
        }
        if (!FindNamedKey(keyName, pScancode))
        {
            std::wcout << L"Unknown key: " << keyName << std::endl;
            return false;
        }
        *pHasModifier = true;
        return true;
    }

    if (!FindNamedKey(spec, pScancode))
    {
        std::wcout << L"Unknown key: " << spec << std::endl;
        return false;
    }
    return true;
}

void CmdKeyDown(const ConsoleCommandParams& params)
{
    uint8_t scancode, modScancode;
    bool okHasModifier;
    if (!ParseKeySpec(params, params.paramPrefixLength, &scancode, &modScancode, &okHasModifier))
        return;

    if (okHasModifier)
        SetKeyState(modScancode, true, (modScancode == BK_KEY_AR2), KEY_MODIFIER_HOLD_FRAMES);
    SetKeyState(scancode, true, (okHasModifier && modScancode == BK_KEY_AR2), KEY_HOLD_FRAMES);
}

void CmdKeyUp(const ConsoleCommandParams& params)
{
    uint8_t scancode, modScancode;
    bool okHasModifier;
    if (!ParseKeySpec(params, params.paramPrefixLength, &scancode, &modScancode, &okHasModifier))
        return;

    SetKeyState(scancode, false, (okHasModifier && modScancode == BK_KEY_AR2), KEY_MODIFIER_HOLD_FRAMES);
    if (okHasModifier)
        SetKeyState(modScancode, false, false, KEY_HOLD_FRAMES);
}

void CmdKeyClick(const ConsoleCommandParams& params)
{
    uint8_t scancode, modScancode;
    bool okHasModifier;
    if (!ParseKeySpec(params, params.paramPrefixLength, &scancode, &modScancode, &okHasModifier))
        return;

    if (okHasModifier)
        ClickKeyWithModifier(modScancode, scancode);
    else
        ClickKey(scancode);
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
    {
        std::wcout << L" Failed to add breakpoint." << std::endl;
        return;
    }
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, address);
    std::wcout << L"Breakpoint set at " << bufAddr << std::endl;
}

void CmdRemoveBreakpointAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    bool result = Emulator_RemoveCPUBreakpoint(address);
    if (!result)
    {
        std::wcout << L" Failed to remove breakpoint." << std::endl;
        return;
    }
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, address);
    std::wcout << L"Breakpoint removed at " << bufAddr << std::endl;
}

void CmdRemoveAllBreakpoints(const ConsoleCommandParams& /*params*/)
{
    Emulator_RemoveAllBreakpoints();
    std::wcout << L"All breakpoints removed." << std::endl;
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
    ARGINFO_OCT_MODIFIERS,  // Optional address, then any combination of postfix modifiers
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
    { L"r ext", ARGINFO_NONE,     CmdPrintExtendedRegisters },     // r ext
    { L"info floppy", ARGINFO_NONE, CmdPrintFloppyRegisters },     // info floppy
    { L"i floppy", ARGINFO_NONE,  CmdPrintFloppyRegisters },       // i floppy
    { L"info",     ARGINFO_NONE,  CmdShowStatus },                 // info
    { L"i",        ARGINFO_NONE,  CmdShowStatus },                 // i
    { L"regs",  ARGINFO_NONE,    CmdPrintAllRegisters },          // regs
    { L"r",     ARGINFO_NONE,    CmdPrintAllRegisters },          // r

    { L"next",    ARGINFO_NONE,    CmdStepOver },
    { L"n",       ARGINFO_NONE,    CmdStepOver },
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
    { L"diskA a", ARGINFO_FILENAME,      CmdAttachFloppyImage },     // diskA a FILENAME
    { L"diskB a", ARGINFO_FILENAME,      CmdAttachFloppyImage },     // diskB a FILENAME
    { L"diskC a", ARGINFO_FILENAME,      CmdAttachFloppyImage },     // diskC a FILENAME
    { L"diskD a", ARGINFO_FILENAME,      CmdAttachFloppyImage },     // diskD a FILENAME
    { L"diskA detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // diskA detach
    { L"diskB detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // diskB detach
    { L"diskC detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // diskC detach
    { L"diskD detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // diskD detach
    { L"diskA d", ARGINFO_NONE,          CmdDetachFloppyImage },     // diskA d
    { L"diskB d", ARGINFO_NONE,          CmdDetachFloppyImage },     // diskB d
    { L"diskC d", ARGINFO_NONE,          CmdDetachFloppyImage },     // diskC d
    { L"diskD d", ARGINFO_NONE,          CmdDetachFloppyImage },     // diskD d

    { L"screenc", ARGINFO_OPT_FILENAME, CmdScreenshot },
    { L"screen",  ARGINFO_OPT_FILENAME, CmdScreenshot },

    { L"monitor", ARGINFO_NONE,   CmdGotoMonitor },
    { L"mo",      ARGINFO_NONE,   CmdGotoMonitor },

    { L"key down", ARGINFO_FILENAME, CmdKeyDown },   // key down KEY, key down MOD+KEY
    { L"kd",       ARGINFO_FILENAME, CmdKeyDown },   // kd KEY, kd MOD+KEY
    { L"key up",   ARGINFO_FILENAME, CmdKeyUp },     // key up KEY, key up MOD+KEY
    { L"ku",       ARGINFO_FILENAME, CmdKeyUp },     // ku KEY, ku MOD+KEY
    { L"key",      ARGINFO_FILENAME, CmdKeyClick },  // key KEY, key MOD+KEY
    { L"k",        ARGINFO_FILENAME, CmdKeyClick },  // k KEY, k MOD+KEY

    { L"tc",    ARGINFO_NONE,    CmdClearTraceLog },              // tc
    { L"trace clear", ARGINFO_NONE, CmdClearTraceLog },           // trace clear
    { L"t clear", ARGINFO_NONE,  CmdClearTraceLog },               // t clear
    { L"trace ", ARGINFO_OCT,    CmdTraceLogWithMask },           // trace XXXXXX
    { L"t",     ARGINFO_OCT,     CmdTraceLogWithMask },           // tXXXXXX
    { L"trace", ARGINFO_NONE,    CmdTraceLogOnOff },              // trace
    { L"t",     ARGINFO_NONE,    CmdTraceLogOnOff },              // t
    { L"examine", ARGINFO_OCT_MODIFIERS, CmdExamineMemory },      // examine [XXXXXX] [bytes] [hex] [nochars]
    { L"x",       ARGINFO_OCT_MODIFIERS, CmdExamineMemory },      // x[XXXXXX] [bytes] [hex] [nochars]

    { L"continue frames ", ARGINFO_DEC, CmdRunFrames },             // continue frames N (decimal)
    { L"continue ", ARGINFO_OCT, CmdRunToAddress },                 // continue XXXXXX
    { L"continue",  ARGINFO_NONE, CmdRun },                         // continue
    { L"c",     ARGINFO_OCT,     CmdRunToAddress },                 // cXXXXXX
    { L"c",     ARGINFO_NONE,    CmdRun },                          // c

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
    params.paramPrefixLength = prefix.size();

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

    case ARGINFO_OCT_MODIFIERS:
        {
            // Optional address -- either glued directly to the prefix
            // ("x100260") or separated by one space ("x 100260") -- then
            // zero or more space-separated modifier words in any
            // order/combination ("bytes", "hex", "nochars"), e.g.:
            //   "x", "x100260", "x hex", "x100260 bytes hex nochars",
            //   "x 100260 bytes hex nochars"
            size_t pos = 0;

            // Peek at the first token, whether glued (no leading space) or
            // separated by one space, and check if it's a genuine octal
            // address before committing to consuming it -- "x bytes" must
            // NOT mistake the leading space for "address then modifiers"
            // when there's no address at all.
            size_t addrStart = (pos < rest.size() && rest[pos] == L' ') ? pos + 1 : pos;
            size_t addrEnd = rest.find(L' ', addrStart);
            std::wstring addrToken = (addrEnd == std::wstring::npos) ? rest.substr(addrStart) : rest.substr(addrStart, addrEnd - addrStart);

            bool okAddrToken = !addrToken.empty();
            for (wchar_t ch : addrToken)
                if (ch < L'0' || ch > L'7') { okAddrToken = false; break; }

            if (okAddrToken)
            {
                uint16_t value = 0;
                for (wchar_t ch : addrToken)
                    value = (uint16_t)((value << 3) + (ch - L'0'));
                params.paramOct1 = value;
                params.paramHasAddress = true;
                pos = addrStart + addrToken.size();
            }
            // else: no valid address present: leave pos at 0, so the
            // modifier loop below sees any leading space and token itself.

            // Remaining modifier words, space-separated, any order.
            while (pos < rest.size())
            {
                if (rest[pos] != L' ')
                    return false;
                pos++;  // skip the space
                size_t wordEnd = rest.find(L' ', pos);
                std::wstring word = (wordEnd == std::wstring::npos) ? rest.substr(pos) : rest.substr(pos, wordEnd - pos);
                if (word.empty())
                    return false;  // double space or trailing space -- reject rather than silently ignore

                if (word == L"bytes")
                    params.paramFlags |= EXAMFLAG_BYTES;
                else if (word == L"hex")
                    params.paramFlags |= EXAMFLAG_HEX;
                else if (word == L"nochars")
                    params.paramFlags |= EXAMFLAG_NOCHARS;
                else
                    return false;  // Unknown modifier word

                pos = (wordEnd == std::wstring::npos) ? rest.size() : wordEnd;
            }
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
    Console_ColorPrompt();
    PrintOctalValue(bufAddr, pProc->GetPC());
    std::wcout << bufAddr << L"> ";
    Console_ColorReset();
}

bool HasPendingContinuation()
{
    return g_continuation.kind != ContinuationKind::None;
}

void ClearPendingContinuation()
{
    g_continuation.kind = ContinuationKind::None;
}

// Print the next page for the armed continuation (examine or disasm),
// picking up exactly where the previous page left off, and re-arm for
// the page after that. Does nothing if no continuation is armed.
void RunPendingContinuation()
{
    if (g_continuation.kind == ContinuationKind::None)
        return;

    ContinuationState state = g_continuation;  // Local copy: handlers below overwrite g_continuation
    CProcessor* pProc = GetCurrentProcessor();

    if (state.kind == ContinuationKind::Examine)
    {
        uint16_t nextAddress;
        PrintMemoryDumpGeneric(pProc, state.address, state.examineFlags, 8, &nextAddress);
        state.address = nextAddress;
        ArmContinuation(state);
    }
    else if (state.kind == ContinuationKind::Disasm)
    {
        uint16_t nextAddress;
        PrintDisassemble(pProc, state.address, false, state.disasmShort, &nextAddress);
        state.address = nextAddress;
        ArmContinuation(state);
    }
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
