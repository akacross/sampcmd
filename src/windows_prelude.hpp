// Centralized Windows include configuration.
//
// Every translation unit that touches the Win32 API includes this FIRST so the
// preprocessor state (Unicode, target version, lean headers) is identical
// everywhere. Getting this wrong is a classic source of subtle ABI/behavior
// differences, and those differences bite hardest under Wine.
#pragma once

#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef _UNICODE
#define _UNICODE 1
#endif

// Target Windows 7+. This only gates which declarations the headers expose; we
// still resolve anything version-sensitive dynamically via GetProcAddress so the
// binary keeps running on older Wine builds.
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef STRICT
#define STRICT
#endif

#include <windows.h>
