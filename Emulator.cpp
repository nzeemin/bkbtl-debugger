/*  This file is part of BKBTL.
    BKBTL is free software: you can redistribute it and/or modify it under the terms
of the GNU Lesser General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
    BKBTL is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU Lesser General Public License for more details.
    You should have received a copy of the GNU Lesser General Public License along with
BKBTL. If not, see <http://www.gnu.org/licenses/>. */

// Emulator.cpp

#include "stdafx.h"
#include "bkbtldebug.h"
#include "Emulator.h"
#include "emubase/Emubase.h"


//////////////////////////////////////////////////////////////////////


CMotherboard* g_pBoard = nullptr;
BKConfiguration g_nEmulatorConfiguration;  // Current configuration

static bool g_okEmulatorInitialized = false;
bool g_okEmulatorRunning = false;

int m_wEmulatorCPUBpsCount = 0;
uint16_t m_EmulatorCPUBps[MAX_BREAKPOINTCOUNT + 1];
uint16_t m_wEmulatorTempCPUBreakpoint = 0177777;

static bool m_okEmulatorSound = false;

static int m_nTickCount = 0;
static uint32_t m_dwEmulatorUptime = 0;  // BK uptime, seconds, from turn on or reset, increments every 25 frames
static long m_nUptimeFrameCount = 0;

uint8_t* g_pEmulatorRam;  // RAM values - for change tracking
uint8_t* g_pEmulatorChangedRam;  // RAM change flags
uint16_t g_wEmulatorCpuR[9] =  // Current register values - R0..R7, PSW
    { 0177777, 0177777, 0177777, 0177777, 0177777, 0177777, 0177777, 0177777, 0177777 };
uint16_t g_wEmulatorPrevCpuR[9];  // Previous register values - R0..R7, PSW

static const int KEYEVENT_QUEUE_SIZE = 32;
static uint16_t m_EmulatorKeyQueue[KEYEVENT_QUEUE_SIZE];
static int m_EmulatorKeyQueueTop = 0;
static int m_EmulatorKeyQueueBottom = 0;
static int m_EmulatorKeyQueueCount = 0;

void CALLBACK Emulator_TeletypeCallback(uint8_t symbol);

void CALLBACK Emulator_FeedDAC(unsigned short l, unsigned short r);

//Прототип функции преобразования экрана
// Input:
//   pVideoBuffer   Исходные данные, биты экрана БК
//   okSmallScreen  Признак "малого" экрана
//   pPalette       Палитра
//   scroll         Текущее значение скроллинга
//   pImageBits     Результат, 32-битный цвет, размер для каждой функции свой
typedef void (CALLBACK* PREPARE_SCREEN_CALLBACK)(const uint8_t* pVideoBuffer, int okSmallScreen, const uint32_t* pPalette, int scroll, void* pImageBits);

void CALLBACK Emulator_PrepareScreenBW512x256(const uint8_t* pVideoBuffer, int okSmallScreen, const uint32_t* pPalette, int scroll, void* pImageBits);
void CALLBACK Emulator_PrepareScreenColor512x256(const uint8_t* pVideoBuffer, int okSmallScreen, const uint32_t* pPalette, int scroll, void* pImageBits);

struct ScreenModeStruct
{
    int width;
    int height;
    PREPARE_SCREEN_CALLBACK callback;
}
static ScreenModeReference[] =
{
    { 512, 256, Emulator_PrepareScreenBW512x256 },
    { 512, 256, Emulator_PrepareScreenColor512x256 },
};

//////////////////////////////////////////////////////////////////////


static const char * FILENAME_BKROM_MONIT10    = "monit10.rom";
static const char * FILENAME_BKROM_FOCAL      = "focal.rom";
static const char * FILENAME_BKROM_TESTS      = "tests.rom";
static const char * FILENAME_BKROM_BASIC10_1  = "basic10_1.rom";
static const char * FILENAME_BKROM_BASIC10_2  = "basic10_2.rom";
static const char * FILENAME_BKROM_BASIC10_3  = "basic10_3.rom";
static const char * FILENAME_BKROM_DISK_326   = "disk_326.rom";
static const char * FILENAME_BKROM_BK11M_BOS  = "b11m_bos.rom";
static const char * FILENAME_BKROM_BK11M_EXT  = "b11m_ext.rom";
static const char * FILENAME_BKROM_BASIC11M_0 = "basic11m_0.rom";
static const char * FILENAME_BKROM_BASIC11M_1 = "basic11m_1.rom";
static const char * FILENAME_BKROM_BK11M_MSTD = "b11m_mstd.rom";


//////////////////////////////////////////////////////////////////////
// Colors

const uint32_t ScreenView_BWPalette[4] =
{
    0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF
};

const uint32_t ScreenView_ColorPalette[4] =
{
    0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFFFF0000
};

const uint32_t ScreenView_ColorPalettes[16][4] =
{
    //                                         Palette#     01           10          11
    { 0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFFFF0000 },  // 00    синий   |   зеленый  |  красный
    { 0xFF000000, 0xFFFFFF00, 0xFFFF00FF, 0xFFFF0000 },  // 01   желтый   |  сиреневый |  красный
    { 0xFF000000, 0xFF00FFFF, 0xFF0000FF, 0xFFFF00FF },  // 02   голубой  |    синий   | сиреневый
    { 0xFF000000, 0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00 },  // 03   зеленый  |   голубой  |  желтый
    { 0xFF000000, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF },  // 04  сиреневый |   голубой  |   белый
    { 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF },  // 05    белый   |    белый   |   белый
    { 0xFF000000, 0xFFC00000, 0xFF8E0000, 0xFFFF0000 },  // 06  темн-красн| красн-корич|  красный
    { 0xFF000000, 0xFFC0FF00, 0xFF8EFF00, 0xFFFFFF00 },  // 07  салатовый | светл-зелен|  желтый
    { 0xFF000000, 0xFFC000FF, 0xFF8E00FF, 0xFFFF00FF },  // 08  фиолетовый| фиол-синий | сиреневый
    { 0xFF000000, 0xFF8EFF00, 0xFF8E00FF, 0xFF8E0000 },  // 09 светл-зелен| фиол-синий |красн-корич
    { 0xFF000000, 0xFFC0FF00, 0xFFC000FF, 0xFFC00000 },  // 10  салатовый | фиолетовый |темн-красный
    { 0xFF000000, 0xFF00FFFF, 0xFFFFFF00, 0xFFFF0000 },  // 11   голубой  |   желтый   |  красный
    { 0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF00FFFF },  // 12   красный  |   зеленый  |  голубой
    { 0xFF000000, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF },  // 13   голубой  |   желтый   |   белый
    { 0xFF000000, 0xFFFFFF00, 0xFF00FF00, 0xFFFFFFFF },  // 14   желтый   |   зеленый  |   белый
    { 0xFF000000, 0xFF00FFFF, 0xFF00FF00, 0xFFFFFFFF },  // 15   голубой  |   зеленый  |   белый
};


//////////////////////////////////////////////////////////////////////

bool Emulator_LoadRomFile(const char * strFileName, uint8_t* buffer, uint32_t fileOffset, uint32_t bytesToRead)
{
    FILE* fpRomFile = ::fopen(strFileName, "rb");
    if (fpRomFile == nullptr)
        return false;

    ASSERT(bytesToRead <= 8192);
    ::memset(buffer, 0, 8192);

    if (fileOffset > 0)
    {
        ::fseek(fpRomFile, fileOffset, SEEK_SET);
    }

    size_t dwBytesRead = ::fread(buffer, 1, bytesToRead, fpRomFile);
    if (dwBytesRead != bytesToRead)
    {
        ::fclose(fpRomFile);
        return false;
    }

    ::fclose(fpRomFile);

    return true;
}

bool Emulator_Init()
{
    ASSERT(g_pBoard == nullptr);

    CProcessor::Init();

    m_wEmulatorCPUBpsCount = 0;
    for (int i = 0; i <= MAX_BREAKPOINTCOUNT; i++)
        m_EmulatorCPUBps[i] = 0177777;
    //for (int i = 0; i <= MAX_BREAKPOINTCOUNT; i++)
    //{
    //    uint16_t address = Settings_GetDebugBreakpoint(i);
    //    m_EmulatorCPUBps[i] = address;
    //    if (address != 0177777) m_wEmulatorCPUBpsCount = i + 1;
    //}

    g_pBoard = new CMotherboard();

    // Allocate memory for old RAM values
    g_pEmulatorRam = (uint8_t*) ::calloc(65536, 1);
    g_pEmulatorChangedRam = (uint8_t*) ::calloc(65536, 1);

    g_pBoard->Reset();

    //g_sound = new QSoundOut();
    if (m_okEmulatorSound)
    {
        g_pBoard->SetSoundGenCallback(Emulator_FeedDAC);
    }

    m_nUptimeFrameCount = 0;
    m_dwEmulatorUptime = 0;

    g_pBoard->SetTeletypeCallback(Emulator_TeletypeCallback);

    Emulator_OnUpdate();

    g_okEmulatorInitialized = true;
    return true;
}

void Emulator_Done()
{
    ASSERT(g_pBoard != nullptr);

    //// Save breakpoints
    //for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    //    Settings_SetDebugBreakpoint(i, i < m_wEmulatorCPUBpsCount ? m_EmulatorCPUBps[i] : 0177777);

    CProcessor::Done();

    g_pBoard->SetSoundGenCallback(nullptr);
    //if (g_sound)
    //{
    //    delete g_sound;
    //    g_sound = nullptr;
    //}

    delete g_pBoard;
    g_pBoard = nullptr;

    // Free memory used for old RAM values
    ::free(g_pEmulatorRam);
    ::free(g_pEmulatorChangedRam);

    g_okEmulatorInitialized = false;
}

bool Emulator_InitConfiguration(BKConfiguration configuration)
{
    g_pBoard->SetConfiguration(configuration);

    uint8_t buffer[8192];

    if ((configuration & BK_COPT_BK0011) == 0)
    {
        // Load Monitor ROM file
        if (!Emulator_LoadRomFile(FILENAME_BKROM_MONIT10, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load Monitor ROM file."));
            return false;
        }
        g_pBoard->LoadROM(0, buffer);
    }

    if (configuration & BK_COPT_ROM_BASIC)
    {
        // Load BASIC ROM 1 file
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BASIC10_1, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load BASIC ROM 1 file."));
            return false;
        }
        g_pBoard->LoadROM(1, buffer);
        // Load BASIC ROM 2 file
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BASIC10_2, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load BASIC ROM 2 file."));
            return false;
        }
        g_pBoard->LoadROM(2, buffer);
        // Load BASIC ROM 3 file
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BASIC10_3, buffer, 0, 8064))
        {
            AlertWarning(_T("Failed to load BASIC ROM 3 file."));
            return false;
        }
        g_pBoard->LoadROM(3, buffer);
    }
    else if (configuration & BK_COPT_ROM_FOCAL)
    {
        // Load Focal ROM file
        if (!Emulator_LoadRomFile(FILENAME_BKROM_FOCAL, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load Focal ROM file."));
            return false;
        }
        g_pBoard->LoadROM(1, buffer);
        // Unused 8KB
        ::memset(buffer, 0, 8192);
        g_pBoard->LoadROM(2, buffer);
        // Load Tests ROM file
        if (!Emulator_LoadRomFile(FILENAME_BKROM_TESTS, buffer, 0, 8064))
        {
            AlertWarning(_T("Failed to load Tests ROM file."));
            return false;
        }
        g_pBoard->LoadROM(3, buffer);
    }

    if (configuration & BK_COPT_BK0011)
    {
        // Load BK0011M BASIC 0, part 1
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BASIC11M_0, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load BK11M BASIC 0 ROM file."));
            return false;
        }
        g_pBoard->LoadROM(0, buffer);
        // Load BK0011M BASIC 0, part 2
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BASIC11M_0, buffer, 8192, 8192))
        {
            AlertWarning(_T("Failed to load BK11M BASIC 0 ROM file."));
            return false;
        }
        g_pBoard->LoadROM(1, buffer);
        // Load BK0011M BASIC 1
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BASIC11M_1, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load BK11M BASIC 1 ROM file."));
            return false;
        }
        g_pBoard->LoadROM(2, buffer);

        // Load BK0011M EXT
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BK11M_EXT, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load BK11M EXT ROM file."));
            return false;
        }
        g_pBoard->LoadROM(3, buffer);
        // Load BK0011M BOS
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BK11M_BOS, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load BK11M BOS ROM file."));
            return false;
        }
        g_pBoard->LoadROM(4, buffer);
    }

    if (configuration & BK_COPT_FDD)
    {
        // Load disk driver ROM file
        ::memset(buffer, 0, 8192);
        if (!Emulator_LoadRomFile(FILENAME_BKROM_DISK_326, buffer, 0, 4096))
        {
            AlertWarning(_T("Failed to load DISK ROM file."));
            return false;
        }
        g_pBoard->LoadROM((configuration & BK_COPT_BK0011) ? 5 : 3, buffer);
    }

    if ((configuration & BK_COPT_BK0011) && (configuration & BK_COPT_FDD) == 0)
    {
        // Load BK0011M MSTD
        if (!Emulator_LoadRomFile(FILENAME_BKROM_BK11M_MSTD, buffer, 0, 8192))
        {
            AlertWarning(_T("Failed to load BK11M MSTD ROM file."));
            return false;
        }
        g_pBoard->LoadROM(5, buffer);
    }

    g_nEmulatorConfiguration = configuration;

    g_pBoard->Reset();

    m_nUptimeFrameCount = 0;
    m_dwEmulatorUptime = 0;

    return true;
}

void Emulator_Start()
{
    g_okEmulatorRunning = true;

    m_nTickCount = 0;

    // For proper breakpoint processing
    if (m_wEmulatorCPUBpsCount != 0)
    {
        g_pBoard->GetCPU()->ClearInternalTick();
    }
}
void Emulator_Stop()
{
    g_okEmulatorRunning = false;

    Emulator_SetTempCPUBreakpoint(0177777);
}

void Emulator_Reset()
{
    ASSERT(g_pBoard != nullptr);

    g_pBoard->Reset();

    m_nUptimeFrameCount = 0;
    m_dwEmulatorUptime = 0;
}

bool Emulator_AddCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorCPUBpsCount == MAX_BREAKPOINTCOUNT - 1 || address == 0177777)
        return false;
    for (int i = 0; i < m_wEmulatorCPUBpsCount; i++)  // Check if the BP exists
    {
        if (m_EmulatorCPUBps[i] == address)
            return false;  // Already in the list
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)  // Put in the first empty cell
    {
        if (m_EmulatorCPUBps[i] == 0177777)
        {
            m_EmulatorCPUBps[i] = address;
            break;
        }
    }
    m_wEmulatorCPUBpsCount++;
    return true;
}
bool Emulator_RemoveCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorCPUBpsCount == 0 || address == 0177777)
        return false;
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorCPUBps[i] == address)
        {
            m_EmulatorCPUBps[i] = 0177777;
            m_wEmulatorCPUBpsCount--;
            if (m_wEmulatorCPUBpsCount > i)  // fill the hole
            {
                m_EmulatorCPUBps[i] = m_EmulatorCPUBps[m_wEmulatorCPUBpsCount];
                m_EmulatorCPUBps[m_wEmulatorCPUBpsCount] = 0177777;
            }
            return true;
        }
    }
    return false;
}
void Emulator_SetTempCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorTempCPUBreakpoint != 0177777)
        Emulator_RemoveCPUBreakpoint(m_wEmulatorTempCPUBreakpoint);
    if (address == 0177777)
    {
        m_wEmulatorTempCPUBreakpoint = 0177777;
        return;
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorCPUBps[i] == address)
            return;  // We have regular breakpoint with the same address
    }
    m_wEmulatorTempCPUBreakpoint = address;
    m_EmulatorCPUBps[m_wEmulatorCPUBpsCount] = address;
    m_wEmulatorCPUBpsCount++;
}
const uint16_t* Emulator_GetCPUBreakpointList() { return m_EmulatorCPUBps; }
bool Emulator_IsBreakpoint()
{
    uint16_t address = g_pBoard->GetCPU()->GetPC();
    if (m_wEmulatorCPUBpsCount > 0)
    {
        for (int i = 0; i < m_wEmulatorCPUBpsCount; i++)
        {
            if (address == m_EmulatorCPUBps[i])
                return true;
        }
    }
    return false;
}
bool Emulator_IsBreakpoint(uint16_t address)
{
    if (m_wEmulatorCPUBpsCount == 0)
        return false;
    for (int i = 0; i < m_wEmulatorCPUBpsCount; i++)
    {
        if (address == m_EmulatorCPUBps[i])
            return true;
    }
    return false;
}
void Emulator_RemoveAllBreakpoints()
{
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
        m_EmulatorCPUBps[i] = 0177777;
    m_wEmulatorCPUBpsCount = 0;
}

bool Emulator_SystemFrame()
{
    Emulator_ProcessKeyEvent();

    g_pBoard->SetCPUBreakpoints(m_wEmulatorCPUBpsCount > 0 ? m_EmulatorCPUBps : nullptr);

    if (!g_pBoard->SystemFrame())  // Breakpoint hit
        return false;

    // Calculate emulator uptime (25 frames per second)
    m_nUptimeFrameCount++;
    if (m_nUptimeFrameCount >= 25)
    {
        m_dwEmulatorUptime++;
        m_nUptimeFrameCount = 0;

        //Global_showUptime(m_dwEmulatorUptime);
    }

    return true;
}

float Emulator_GetUptime()
{
    return (float)m_dwEmulatorUptime + float(m_nUptimeFrameCount) / 25.0f;
}

// Update cached values after Run or Step
void Emulator_OnUpdate()
{
    // Update stored PC value
    for (int r = 0; r < 9; r++)
        g_wEmulatorPrevCpuR[r] = g_wEmulatorCpuR[r];
    for (int r = 0; r < 8; r++)
        g_wEmulatorCpuR[r] = g_pBoard->GetCPU()->GetReg(r);
    g_wEmulatorCpuR[8] = g_pBoard->GetCPU()->GetPSW();

    // Update memory change flags
    {
        uint8_t* pOld = g_pEmulatorRam;
        uint8_t* pChanged = g_pEmulatorChangedRam;
        uint16_t addr = 0;
        do
        {
            uint8_t newvalue = g_pBoard->GetRAMByte(addr);
            uint8_t oldvalue = *pOld;
            *pChanged = (newvalue != oldvalue) ? 255 : 0;
            *pOld = newvalue;
            addr++;
            pOld++;  pChanged++;
        }
        while (addr < 65535);
    }
}

bool Emulator_IsRegisterChanged(int r)
{
    return g_wEmulatorPrevCpuR[r] != g_wEmulatorCpuR[r];
}

// Get RAM change flag
//   addrtype - address mode - see ADDRTYPE_XXX constants
uint16_t Emulator_GetChangeRamStatus(uint16_t address)
{
    return *((uint16_t*)(g_pEmulatorChangedRam + address));
}

void Emulator_GetScreenSize(int scrmode, int* pwid, int* phei)
{
    if (scrmode < 0 || scrmode >= int(sizeof(ScreenModeReference) / sizeof(ScreenModeStruct)))
        return;
    ScreenModeStruct* pinfo = ScreenModeReference + scrmode;
    *pwid = pinfo->width;
    *phei = pinfo->height;
}

void Emulator_PrepareScreenRGB32(void* pImageBits, int screenMode)
{
    if (pImageBits == nullptr) return;
    if (!g_okEmulatorInitialized) return;

    // Get scroll value
    uint16_t scroll = g_pBoard->GetPortView(0177664);
    bool okSmallScreen = ((scroll & 01000) == 0);
    scroll &= 0377;
    scroll = (scroll >= 0330) ? scroll - 0330 : 050 + scroll;

    // Get palette
    uint32_t* pPalette;
    if ((g_nEmulatorConfiguration & BK_COPT_BK0011) == 0)
        pPalette = (uint32_t*)ScreenView_ColorPalette;
    else
        pPalette = (uint32_t*)ScreenView_ColorPalettes[g_pBoard->GetPalette()];

    const uint8_t* pVideoBuffer = g_pBoard->GetVideoBuffer();
    ASSERT(pVideoBuffer != nullptr);

    // Render to bitmap
    PREPARE_SCREEN_CALLBACK callback = ScreenModeReference[screenMode].callback;
    callback(pVideoBuffer, okSmallScreen, pPalette, scroll, pImageBits);
}

//#define AVERAGERGB(a, b)  ( ((((a) & 0x00fefeffUL) + ((b) & 0x00fefeffUL)) >> 1) | 0xff000000 )

void CALLBACK Emulator_PrepareScreenBW512x256(const uint8_t* pVideoBuffer, int okSmallScreen, const uint32_t* /*pPalette*/, int scroll, void* pImageBits)
{
    int linesToShow = okSmallScreen ? 64 : 256;
    for (int y = 0; y < linesToShow; y++)
    {
        int yy = (y + scroll) & 0377;
        const uint16_t* pVideo = (uint16_t*)(pVideoBuffer + yy * 0100);
        uint32_t* pBits = (uint32_t*)pImageBits + y * 512;
        for (int x = 0; x < 512 / 16; x++)
        {
            uint16_t src = *pVideo;

            for (int bit = 0; bit < 16; bit++)
            {
                uint32_t color = (src & 1) ? 0xffffffff : 0xff000000;
                *pBits = color;
                pBits++;
                src = src >> 1;
            }

            pVideo++;
        }
    }
    if (okSmallScreen)
    {
        uint32_t* pBits = (uint32_t*)pImageBits;
        for (int i = 0; i < (256 - 64) * 512; i++)
            *pBits++ = 0xff000000;
    }
}

void CALLBACK Emulator_PrepareScreenColor512x256(const uint8_t* pVideoBuffer, int okSmallScreen, const uint32_t* pPalette, int scroll, void* pImageBits)
{
    int linesToShow = okSmallScreen ? 64 : 256;
    for (int y = 0; y < linesToShow; y++)
    {
        int yy = (y + scroll) & 0377;
        const uint16_t* pVideo = (uint16_t*)(pVideoBuffer + yy * 0100);
        uint32_t* pBits = (uint32_t*)pImageBits + y * 512;
        for (int x = 0; x < 512 / 16; x++)
        {
            uint16_t src = *pVideo;

            for (int bit = 0; bit < 16; bit += 2)
            {
                uint32_t color = pPalette[src & 3];
                *pBits = color;
                pBits++;
                *pBits = color;
                pBits++;
                src = src >> 2;
            }

            pVideo++;
        }
    }
    if (okSmallScreen)
    {
        uint32_t* pBits = (uint32_t*)pImageBits;
        for (int i = 0; i < (256 - 64) * 512; i++)
            *pBits++ = 0xff000000;
    }
}

void Emulator_KeyEvent(uint8_t keyscan, bool pressed, bool ctrl)
{
    if (m_EmulatorKeyQueueCount == KEYEVENT_QUEUE_SIZE) return;  // Full queue

    uint16_t keyflags = (pressed ? 128 : 0) | (ctrl ? 64 : 0);
    uint16_t keyevent = ((uint16_t)keyscan) | (keyflags << 8);

    m_EmulatorKeyQueue[m_EmulatorKeyQueueTop] = keyevent;
    m_EmulatorKeyQueueTop++;
    if (m_EmulatorKeyQueueTop >= KEYEVENT_QUEUE_SIZE)
        m_EmulatorKeyQueueTop = 0;
    m_EmulatorKeyQueueCount++;
}

uint16_t Emulator_GetKeyEventFromQueue()
{
    if (m_EmulatorKeyQueueCount == 0) return 0;  // Empty queue

    uint16_t keyevent = m_EmulatorKeyQueue[m_EmulatorKeyQueueBottom];
    m_EmulatorKeyQueueBottom++;
    if (m_EmulatorKeyQueueBottom >= KEYEVENT_QUEUE_SIZE)
        m_EmulatorKeyQueueBottom = 0;
    m_EmulatorKeyQueueCount--;

    return keyevent;
}

void Emulator_ProcessKeyEvent()
{
    // Process next event in the keyboard queue
    uint16_t keyevent = Emulator_GetKeyEventFromQueue();
    if (keyevent != 0)
    {
        bool pressed = ((keyevent & 0x8000) != 0);
        bool ctrl = ((keyevent & 0x4000) != 0);
        uint8_t bkscan = (uint8_t)(keyevent & 0xff);
        g_pBoard->KeyboardEvent(bkscan, pressed, ctrl);
    }
}

void CALLBACK Emulator_FeedDAC(unsigned short l, unsigned short r)
{
    //if (g_sound)
    //{
    //    if (m_okEmulatorSound)
    //        g_sound->FeedDAC(l, r);
    //}
}

void Emulator_SetSound(bool enable)
{
    m_okEmulatorSound = enable;
    if (g_pBoard != nullptr)
    {
        if (enable)
            g_pBoard->SetSoundGenCallback(Emulator_FeedDAC);
        else
            g_pBoard->SetSoundGenCallback(nullptr);
    }
}

void CALLBACK Emulator_TeletypeCallback(uint8_t symbol)
{
    if (symbol >= 32 || symbol == 13 || symbol == 10)
    {
        //Global_getMainWindow()->printToTeletype(std::string((TCHAR)symbol));
    }
    else
    {
        //char buffer[32];
        //_snprintf(buffer, 32, "<%02x>", symbol);
        //Global_getMainWindow()->printToTeletype(buffer);
    }
}


//////////////////////////////////////////////////////////////////////
//
// Emulator image format - see CMotherboard::SaveToImage()
// Image header format (32 bytes):
//   4 bytes        BK_IMAGE_HEADER1
//   4 bytes        BK_IMAGE_HEADER2
//   4 bytes        BK_IMAGE_VERSION
//   4 bytes        BK_IMAGE_SIZE
//   4 bytes        BK uptime
//   12 bytes       Not used

bool Emulator_SaveImage(const std::string &sFilePath)
{
    std::ofstream file(sFilePath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!file.is_open())
    {
        AlertWarning(_T("Failed to save image file."));
        return false;
    }

    // Allocate memory
    uint8_t* pImage = (uint8_t*) ::calloc(BKIMAGE_SIZE, 1);
    if (pImage == nullptr)
    {
        file.close();
        return false;
    }
    // Prepare header
    uint32_t* pHeader = (uint32_t*) pImage;
    *pHeader++ = BKIMAGE_HEADER1;
    *pHeader++ = BKIMAGE_HEADER2;
    *pHeader++ = BKIMAGE_VERSION;
    *pHeader++ = BKIMAGE_SIZE;
    // Store emulator state to the image
    g_pBoard->SaveToImage(pImage);
    *(uint32_t*)(pImage + 16) = m_dwEmulatorUptime;

    // Save image to the file
    file.write(reinterpret_cast<const char*>(pImage), BKIMAGE_SIZE);
    if (!file)
    {
        AlertWarning(_T("Failed to save image file data."));
        return false;
    }

    // Free memory, close file
    ::free(pImage);
    file.close();

    return true;
}

bool Emulator_LoadImage(const std::string &sFilePath)
{
    Emulator_Stop();

    std::ifstream file(sFilePath, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        AlertWarning(_T("Failed to load image file."));
        return false;
    }

    // Read header
    uint32_t bufHeader[BKIMAGE_HEADER_SIZE / sizeof(uint32_t)];
    file.read((char*)bufHeader, BKIMAGE_HEADER_SIZE);
    if (!file)
    {
        file.close();
        return false;
    }

    //TODO: Check version and size

    // Allocate memory
    uint8_t* pImage = (uint8_t*) ::malloc(BKIMAGE_SIZE);
    if (pImage == nullptr)
    {
        file.close();
        return false;
    }

    // Read image
    file.seekg(0);
    file.read((char*)pImage, BKIMAGE_SIZE);
    if (!file)
    {
        ::free(pImage);
        file.close();
        AlertWarning(_T("Failed to load image file data."));
        return false;
    }

    // Restore emulator state from the image
    g_pBoard->LoadFromImage(pImage);

    m_dwEmulatorUptime = *(uint32_t*)(pImage + 16);

    for (int r = 0; r < 8; r++)
        g_wEmulatorCpuR[r] = g_pBoard->GetCPU()->GetReg(r);
    g_wEmulatorCpuR[8] = g_pBoard->GetCPU()->GetPSW();

    // Free memory, close file
    ::free(pImage);
    file.close();

    return true;
}


//////////////////////////////////////////////////////////////////////
