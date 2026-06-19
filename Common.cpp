// Common.cpp

#include "stdafx.h"
#include <cstdarg>
#include "bkbtldebug.h"


//////////////////////////////////////////////////////////////////////


bool AssertFailedLine(const char * lpszFileName, int nLine)
{
    DebugPrintFormat(_T("ASSERT in %s at line %n"), lpszFileName, nLine);

    return false;
}

void AlertInfo(LPCTSTR message)
{
    std::wcout << _T("[INFO] ") << message << std::endl;
}

void AlertWarning(LPCTSTR message)
{
    std::wcout << _T("[WARN] ") << message << std::endl;
}

bool AlertOkCancel(LPCTSTR message)
{
    std::wcout << _T("[ASK] ") << message << std::endl;
    //TODO
    return false;
}


//////////////////////////////////////////////////////////////////////
// DebugPrint and DebugLog

#if !defined(PRODUCT)

void DebugPrint(LPCTSTR message)
{
    std::wcout << message << std::endl;
}

void DebugPrintFormat(LPCTSTR format, ...)
{
    TCHAR buffer[512];

    va_list ptr;
    va_start(ptr, format);
    _vsntprintf_s(buffer, 512, 512 - 1, format, ptr);
    va_end(ptr);

    DebugPrint(buffer);
}

const char* TRACELOG_FILE_NAME = "trace.log";
const char* TRACELOG_NEWLINE = "\r\n";

FILE* Common_LogFile = nullptr;

void DebugLog(LPCTSTR message)
{
    if (Common_LogFile == nullptr)
    {
        Common_LogFile = ::fopen(TRACELOG_FILE_NAME, "a+b");
        //TODO: Check if Common_LogFile == nullptr
    }

    ::fseek(Common_LogFile, 0, SEEK_END);

    size_t dwLength = _tcslen(message) * sizeof(TCHAR);
    ::fwrite(message, 1, dwLength, Common_LogFile);
}

void DebugLogFormat(LPCTSTR pszFormat, ...)
{
    TCHAR buffer[512];

    va_list ptr;
    va_start(ptr, pszFormat);
    _vsntprintf_s(buffer, 512, 512 - 1, pszFormat, ptr);
    va_end(ptr);

    DebugLog(buffer);
}

void DebugLogCloseFile()
{
    if (Common_LogFile != nullptr)
    {
        ::fclose(Common_LogFile);
        Common_LogFile = nullptr;
    }
}

void DebugLogClear()
{
    // Close the handle first if open, since it's positioned at EOF from
    // appending and reopening in "w" mode from here wouldn't affect an
    // already-open append handle held elsewhere.
    DebugLogCloseFile();
    FILE* f = ::fopen(TRACELOG_FILE_NAME, "wb");
    if (f != nullptr)
        ::fclose(f);
}


#endif // !defined(PRODUCT)


//////////////////////////////////////////////////////////////////////


// Processor register names
const TCHAR* REGISTER_NAME[] = { _T("R0"), _T("R1"), _T("R2"), _T("R3"), _T("R4"), _T("R5"), _T("SP"), _T("PC") };


// Print octal 16-bit value to buffer
// buffer size at least 7 characters
void PrintOctalValue(TCHAR* buffer, uint16_t value)
{
    for (int p = 0; p < 6; p++)
    {
        int digit = value & 7;
        buffer[5 - p] = _T('0') + (TCHAR)digit;
        value = (value >> 3);
    }
    buffer[6] = 0;
}
// Print hex 16-bit value to buffer
// buffer size at least 5 characters
void PrintHexValue(TCHAR* buffer, uint16_t value)
{
    for (int p = 0; p < 4; p++)
    {
        int digit = value & 15;
        buffer[3 - p] = (digit < 10) ? _T('0') + (TCHAR)digit : _T('a') + (TCHAR)(digit - 10);
        value = (value >> 4);
    }
    buffer[4] = 0;
}
// Print binary 16-bit value to buffer
// buffer size at least 17 characters
void PrintBinaryValue(TCHAR* buffer, uint16_t value)
{
    for (int b = 0; b < 16; b++)
    {
        int bit = (value >> b) & 1;
        buffer[15 - b] = bit ? _T('1') : _T('0');
    }
    buffer[16] = 0;
}

// Parse 16-bit octal value from text
bool ParseOctalValue(const char* text, uint16_t* pValue)
{
    uint16_t value = 0;
    char* pChar = (char*) text;
    for (int p = 0; ; p++)
    {
        if (p > 6) return false;
        char ch = *pChar;  pChar++;
        if (ch == 0) break;
        if (ch < '0' || ch > '7') return false;
        value = (value << 3);
        int digit = ch - '0';
        value += digit;
    }
    *pValue = value;
    return true;
}

// Parse 16-bit hex value from text
bool ParseHexValue(const char* text, uint16_t* pValue)
{
    uint16_t value = 0;
    char* pChar = (char*) text;
    for (int p = 0; ; p++)
    {
        if (p > 4) return false;
        char ch = *pChar;  pChar++;
        if (ch == 0) break;
        if (ch >= '0' && ch <= '9')
        {
            value = (value << 4);
            int digit = ch - '0';
            value += digit;
        }
        else if (ch >= 'a' && ch <= 'f')
        {
            value = (value << 4);
            int digit = ch - 'a' + 10;
            value += digit;
        }
        else if (ch >= 'A' && ch <= 'F')
        {
            value = (value << 4);
            int digit = ch - 'A' + 10;
            value += digit;
        }
        else
            return false;
    }
    *pValue = value;
    return true;
}


// BK to Unicode conversion table
const uint16_t BK_CHAR_CODES[] =
{
    0x3C0,  0x2534, 0x2665, 0x2510, 0x2561, 0x251C, 0x2514, 0x2550, 0x2564, 0x2660, 0x250C, 0x252C, 0x2568, 0x2193, 0x253C, 0x2551,
    0x2524, 0x2190, 0x256C, 0x2191, 0x2663, 0x2500, 0x256B, 0x2502, 0x2666, 0x2518, 0x256A, 0x2565, 0x2567, 0x255E, 0x2192, 0x2593,
    0x44E,  0x430,  0x431,  0x446,  0x434,  0x435,  0x444,  0x433,  0x445,  0x438,  0x439,  0x43A,  0x43B,  0x43C,  0x43D,  0x43E,
    0x43F,  0x44F,  0x440,  0x441,  0x442,  0x443,  0x436,  0x432,  0x44C,  0x44B,  0x437,  0x448,  0x44D,  0x449,  0x447,  0x44A,
    0x42E,  0x410,  0x411,  0x426,  0x414,  0x415,  0x424,  0x413,  0x425,  0x418,  0x419,  0x41A,  0x41B,  0x41C,  0x41D,  0x41E,
    0x41F,  0x42F,  0x420,  0x421,  0x422,  0x423,  0x416,  0x412,  0x42C,  0x42B,  0x417,  0x428,  0x42D,  0x429,  0x427,  0x42A,
};
// Translate one KOI8-R character to Unicode character
uint16_t Translate_BK_Unicode(uint8_t ch)
{
    if (ch < 32) return 0x00b7;
    if (ch < 127) return (uint16_t) ch;
    if (ch == 127) return (uint16_t) 0x25A0;
    if (ch >= 128 && ch < 160) return 0x00b7;
    return BK_CHAR_CODES[ch - 160];
}


//////////////////////////////////////////////////////////////////////
