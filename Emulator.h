// Emulator.h

#pragma once

#include "bkbtldebug.h"
#include "emubase/Board.h"

//////////////////////////////////////////////////////////////////////

const int MAX_BREAKPOINTCOUNT = 16;

extern CMotherboard* g_pBoard;
extern BKConfiguration g_nEmulatorConfiguration;  // Current configuration

// Directory to look for .rom files in, set via the --romdir command-line
// option. Empty means "current directory" (the historical behavior).
extern std::string g_strRomDirectory;

// True = emulate the K1801VM1G processor (MUL instruction, timer interrupt 0270),
// set via the --vm1g command-line option. Default is the K1801VM1A/B/V chip.
extern bool g_okEmulatorVM1G;
extern bool g_okEmulatorRunning;

extern uint8_t* g_pEmulatorRam;  // RAM values - for change tracking
extern uint8_t* g_pEmulatorChangedRam;  // RAM change flags
extern uint16_t g_wEmulatorCpuR[9];      // Current PC value
extern uint16_t g_wEmulatorPrevCpuR[9];  // Previous PC value

extern const uint32_t ScreenView_BWPalette[4];
extern const uint32_t ScreenView_ColorPalette[4];


//////////////////////////////////////////////////////////////////////

void Emulator_SetSound(bool enable);
bool Emulator_Init();
bool Emulator_InitConfiguration(BKConfiguration configuration);
void Emulator_Done();

bool Emulator_AddCPUBreakpoint(uint16_t address);
bool Emulator_RemoveCPUBreakpoint(uint16_t address);
void Emulator_SetTempCPUBreakpoint(uint16_t address);
const uint16_t* Emulator_GetCPUBreakpointList();
bool Emulator_IsBreakpoint();
bool Emulator_IsBreakpoint(uint16_t address);
void Emulator_RemoveAllBreakpoints();

//void Emulator_SetSound(bool soundOnOff);
//void Emulator_SetCovox(bool covoxOnOff);
void Emulator_Start();
void Emulator_Stop();
void Emulator_Reset();
bool Emulator_SystemFrame();
float Emulator_GetUptime();  // BK uptime, in seconds

void Emulator_GetScreenSize(int scrmode, int* pwid, int* phei);
void Emulator_PrepareScreenRGB32(void* pBits, int screenMode);

void Emulator_KeyEvent(uint8_t keyPressed, bool pressed, bool ctrl);
uint16_t Emulator_GetKeyEventFromQueue();
void Emulator_ProcessKeyEvent();

// Set trace flags (TRACE_xxx)
void Emulator_SetTrace(uint32_t dwTrace);

// Instruction history, recorded always
const int INSTR_HISTORY_SIZE = 256;  // Must be a power of two

// A record is either an executed instruction or an interrupt taken by the processor
struct InstructionRecord
{
    uint16_t pc;        // Instruction: its address; interrupt: PC where the interrupt appeared
    uint16_t words[3];  // Instruction: opcode and two next words, the instruction is at most 3 words long;
                        // interrupt: vector, new PC, new PSW
    uint16_t psw;       // Instruction: PSW before it; interrupt: new PSW
    bool     interrupt; // true = interrupt record
};

int Emulator_GetHistoryLength();
const InstructionRecord& Emulator_GetHistoryRecord(int index);  // index 0 = most recent
void Emulator_ClearHistory();
LPCTSTR Emulator_GetInterruptName(uint16_t vector);  // Short name of the interrupt vector

// Update cached values after Run or Step
void Emulator_OnUpdate();
bool Emulator_IsRegisterChanged(int r);
uint16_t Emulator_GetChangeRamStatus(uint16_t address);

bool Emulator_AttachFloppyImage(int slot, LPCTSTR sFilePath);
void Emulator_DetachFloppyImage(int slot);
bool Emulator_IsFloppyImageAttached(int slot);
bool Emulator_IsFloppyReadOnly(int slot);
bool Emulator_IsFloppyEngineOn();
bool Emulator_SaveImage(const std::string &sFilePath);
bool Emulator_LoadImage(const std::string &sFilePath);


//////////////////////////////////////////////////////////////////////
