#ifndef COMMON_H
#define COMMON_H
// Common.h

#pragma once


//////////////////////////////////////////////////////////////////////
// Defines for compilation under MinGW and GCC

#ifndef _TCHAR_DEFINED
typedef char TCHAR;
#define _tfopen     fopen
#define _tfsopen    _fsopen
#define _tcscpy     strcpy
#define _tcscpy_s   strcpy_s
#define _tstat      _stat
#define _tcsrchr    strrchr
#define _tcsicmp    _stricmp
#define _tcscmp     strcmp
#define _tcslen     strlen
#define _sntprintf  _snprintf
#define _vsntprintf_s(buffer, sizeOfBuffer, count, format, args) vsnprintf(buffer, sizeOfBuffer, format, args)
#define _T(x)       x
typedef char * LPTSTR;
typedef const char * LPCTSTR;
#else
typedef TCHAR* LPTSTR;
typedef const TCHAR* LPCTSTR;
#endif

#ifdef __GNUC__
//#define _stat       stat
#define _stricmp    strcasecmp
#define _snprintf   snprintf
#endif

#ifdef __GNUC__
#define CALLBACK
#else
#define CALLBACK __stdcall
#endif


//////////////////////////////////////////////////////////////////////
// Assertions checking - MFC-like ASSERT macro

#ifdef _DEBUG

#ifdef __GNUC__
#include <csignal>
#define __debugbreak() raise(SIGTRAP)
#endif

bool AssertFailedLine(const char * lpszFileName, int nLine);
#define ASSERT(f)          (void) ((f) || !AssertFailedLine(__FILE__, __LINE__) || (__debugbreak(), 0))
#define VERIFY(f)          ASSERT(f)

#else   // _DEBUG

#define ASSERT(f)          ((void)0)
#define VERIFY(f)          ((void)f)

#endif // !_DEBUG


//////////////////////////////////////////////////////////////////////


void AlertInfo(LPCTSTR message);
void AlertWarning(LPCTSTR message);
bool AlertOkCancel(LPCTSTR message);


//////////////////////////////////////////////////////////////////////
// DebugPrint

#if !defined(PRODUCT)

void DebugPrint(LPCTSTR message);
void DebugPrintFormat(LPCTSTR pszFormat, ...);
void DebugLog(LPCTSTR message);
void DebugLogFormat(LPCTSTR pszFormat, ...);
void DebugLogClear();      // Truncate trace.log to empty
void DebugLogCloseFile();  // Close the trace.log file handle, if open

#endif // !defined(PRODUCT)


//////////////////////////////////////////////////////////////////////


// Processor register names
extern const TCHAR* REGISTER_NAME[];

const int BK_SCREEN_WIDTH = 512;
const int BK_SCREEN_HEIGHT = 256;

void PrintOctalValue(TCHAR* buffer, uint16_t value);
void PrintHexValue(TCHAR* buffer, uint16_t value);
void PrintBinaryValue(TCHAR* buffer, uint16_t value);
bool ParseOctalValue(const char* text, uint16_t* pValue);
bool ParseHexValue(const char* text, uint16_t* pValue);

uint16_t Translate_BK_Unicode(uint8_t ch);


//////////////////////////////////////////////////////////////////////
#endif // COMMON_H
