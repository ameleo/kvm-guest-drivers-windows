#pragma once
#include "kdebugprint.h"

//#define DBG 1

#ifndef TRACE_LEVEL_INFORMATION
#define TRACE_LEVEL_NONE        0   // Tracing is not on
#define TRACE_LEVEL_FATAL       1   // Abnormal exit or termination
#define TRACE_LEVEL_ERROR       2   // Severe errors that need logging
#define TRACE_LEVEL_WARNING     3   // Warnings such as allocation failure
#define TRACE_LEVEL_INFORMATION 4   // Includes non-error cases(e.g.,Entry-Exit)
#define TRACE_LEVEL_VERBOSE     5   // Detailed traces from intermediate steps
#define TRACE_LEVEL_RESERVED6   6
#define TRACE_LEVEL_RESERVED7   7
#define TRACE_LEVEL_RESERVED8   8
#define TRACE_LEVEL_RESERVED9   9
#endif // TRACE_LEVEL_INFORMATION

//
// Route DbgPrint to COM1 (0x3F8) instead of WPP, in a Release build, without turning on DBG.
//
// WPP traces are collected by ETW inside the guest. That is unusable the moment the display path is what is
// being debugged: no screen, no way to start the collector, no trace. The serial writer needs nothing in the
// guest - run QEMU with `-serial file:viogpu-trace.log` and read it on the host.
//
// Turning DBG on would also have done this, but it arms NT_ASSERT everywhere, which turns a display bug into a
// bugcheck. This toggle keeps Release semantics and only changes where the text goes.
//
// Set to 0 to go back to WPP.
//
#define VIOGPU_SERIAL_TRACE 1

#if DBG
//
// CAREFUL: this branch is the one that is actually taken by the shipping build. "Win11 Release|x64" is the only
// configuration in viogpudo.vcxproj with UseDebugLibraries=true, which defines DBG=1 - so the driver is built
// with debug semantics (NT_ASSERT armed included) despite the configuration being named Release.
//
// VIOGPU_SERIAL_TRACE therefore has to steer THIS branch as well, otherwise it silently selects a code path that
// is never compiled: PRINT_DEBUG routes DbgPrint to vDbgPrintEx, i.e. to a kernel debugger, and nothing ever
// reaches the serial port no matter what the #else branch says.
//
#if VIOGPU_SERIAL_TRACE
#define COM_DEBUG 1
#else
#define PRINT_DEBUG 1
#endif

    extern int nDebugLevel;
    extern int bBreakAlways;

    void DebugPrintFuncSerial(const char *format, ...);
    void DebugPrintFuncKdPrint(const char *format, ...);
    void InitializeSerialPort(void);
#define DbgExpandArguments(...) __VA_ARGS__
#define DbgPrint(Level, MSG) \
    if (Level <= nDebugLevel) VirtioDebugPrintProc(DbgExpandArguments MSG);
#define VioGpuDbgBreak()\
    if (KD_DEBUGGER_ENABLED && !KD_DEBUGGER_NOT_PRESENT && bBreakAlways) DbgBreakPoint();

#define WPP_INIT_TRACING(driver, regpath)  InitializeDebugPrints(driver, regpath);
#define WPP_CLEANUP(driver)
#elif VIOGPU_SERIAL_TRACE

extern int nDebugLevel;

void DebugPrintFuncSerial(const char *format, ...);
void InitializeSerialDebugPrints(void);

#define DbgExpandArguments(...) __VA_ARGS__
#define DbgPrint(Level, MSG) \
    if ((Level) <= nDebugLevel) DebugPrintFuncSerial(DbgExpandArguments MSG);

#define VioGpuDbgBreak() {}

// No session to attach to, so tracing is live from the first line of DriverEntry - which is the point: the
// interesting failures happen long before anything in the guest could start a collector.
#define WPP_INIT_TRACING(driver, regpath) InitializeSerialDebugPrints()
#define WPP_CLEANUP(driver)

#else
//#define DbgPrint(level, line) {};

#define WPP_CHECK_FOR_NULL_STRING  // to prevent exceptions due to NULL strings.

// Tracing GUID - {D6B96B2C-72BF-4CA5-BB89-9FCA5C82F020}

#define WPP_CONTROL_GUIDS                                              \
    WPP_DEFINE_CONTROL_GUID(                                           \
        VioGpuDodTraceGuid, (D6B96B2C,72BF,4CA5,BB89,9FCA5C82F020),    \
        WPP_DEFINE_BIT(TRACE_ALL)                                      \
        )

#define WPP_FLAG_LEVEL_LOGGER(flag, level)     WPP_LEVEL_LOGGER(flag)

#define WPP_FLAG_LEVEL_ENABLED(flag, level)                            \
    (WPP_LEVEL_ENABLED(flag) &&                                        \
     WPP_CONTROL(WPP_BIT_ ## flag).Level >= level)

#define WPP_LEVEL_FLAGS_LOGGER(lvl,flags)      WPP_LEVEL_LOGGER(flags)

#define WPP_LEVEL_FLAGS_ENABLED(lvl, flags)                            \
    (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_ ## flags).Level >= lvl)

#define VioGpuDbgBreak() {}

// begin_wpp config
// USEPREFIX (DbgPrint, "%!STDPREFIX! %!FUNC!");
// DbgPrint{FLAG = TRACE_ALL}(LEVEL, (MSG, ...));
// end_wpp
//

#endif
