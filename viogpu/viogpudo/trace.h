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
// Debugging aid, off by default: route DbgPrint to COM1 (0x3F8) instead of the kernel debugger.
//
// When the display path is what is being debugged there is no screen to start a trace collector from. The serial
// writer needs nothing in the guest - run QEMU with `-serial file:viogpu-trace.log` and read it on the host.
// It reprograms COM1 when the driver loads and writes every FATAL trace there, so it must never be on in a build
// given to guests that may use COM1 (a serial console, kdcom).
//
// It only takes effect in a DBG build. "Win11 Release|x64" is one (see below); the other configurations use WPP.
//
#ifndef VIOGPU_SERIAL_TRACE
#define VIOGPU_SERIAL_TRACE 0
#endif

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
