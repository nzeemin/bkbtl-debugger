// stdafx.h : Pre-compiled header
//

#pragma once

#ifdef _MSC_VER
//NOTE: I know, we use unsafe string copy functions
#define _CRT_SECURE_NO_WARNINGS
#endif

// C RunTime Header Files
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <cstring>
#include <vector>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <locale>
#include <filesystem>

namespace fs = std::filesystem;

#ifdef _MSC_VER
#include <io.h>
#include <fcntl.h>
#endif

#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif

#include <memory.h>

#ifndef __GNUC__
#include <tchar.h>
#endif

#include "Common.h"
