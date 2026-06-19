// Emulator.h

#pragma once

#include "bkbtldebug.h"
#include "emubase/Board.h"

//////////////////////////////////////////////////////////////////////

const int MAX_BREAKPOINTCOUNT = 16;

extern CMotherboard* g_pBoard;
extern BKConfiguration g_nEmulatorConfiguration;  // Current configuration
extern bool g_okEmulatorRunning;

extern uint8_t* g_pEmulatorRam;  // RAM values - for change tracking
extern uint8_t* g_pEmulatorChangedRam;  // RAM change flags
extern uint16_t g_wEmulatorCpuPC;      // Current PC value
extern uint16_t g_wEmulatorPrevCpuPC;  // Previous PC value
extern uint16_t g_wEmulatorPpuPC;      // Current PC value
extern uint16_t g_wEmulatorPrevPpuPC;  // Previous PC value


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

// Update cached values after Run or Step
void Emulator_OnUpdate();
uint16_t Emulator_GetChangeRamStatus(uint16_t address);

bool Emulator_SaveImage(const std::string &sFilePath);
bool Emulator_LoadImage(const std::string &sFilePath);


//////////////////////////////////////////////////////////////////////
