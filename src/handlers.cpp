/**
 * @file    handlers.cpp
 * @brief   OS-specific and generic C++ error/signal/whatever handlers
 */

// public library header
#include "ooopsi.hpp"
// private library header
#include "internal.hpp"

#include <system_error>
#include <tuple> // for std::ignore

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/ucontext.h>
#endif // _WIN32

#include <csignal>
#include <cstring>

namespace ooopsi
{


#ifdef _WIN32 // Windows-specific handlers

/**
 * Abort handler for Windows.
 * @param[in] excInfo      information about the current exception
 */
static LONG WINAPI onWindowsException(EXCEPTION_POINTERS* excInfo)
{
    const char* what = "???";
    const char* detail = nullptr;
    char detailBuf[64];
    const auto& excRec = *excInfo->ExceptionRecord;
    const uintptr_t* addr = nullptr;

    // TODO: how to get the function name?
    printf("winExc, addr=%llx\n", excRec.ExceptionAddress);
    switch (excRec.ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        what = "SEGMENTATION FAULT";
        if (excRec.NumberParameters >= 2)
        {
            // the first element contains a read/write flag
            // the second element contains the virtual address of the inaccessible data
            addr = &excRec.ExceptionInformation[1];
        }
        break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        what = "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        break;
    case EXCEPTION_BREAKPOINT:
        what = "EXCEPTION_BREAKPOINT";
        break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        what = "EXCEPTION_DATATYPE_MISALIGNMENT";
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        what = "FLOATING POINT ERROR";
        detail = "floating-point denormal operand";
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        what = "FLOATING POINT ERROR";
        detail = "floating-point divide by zero";
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        what = "FLOATING POINT ERROR";
        detail = " (floating-point inexact result)";
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        what = "FLOATING POINT ERROR";
        detail = "floating-point invalid operation";
        break;
    case EXCEPTION_FLT_OVERFLOW:
        what = "FLOATING POINT ERROR";
        detail = "floating-point overflow";
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        what = "FLOATING POINT ERROR";
        detail = "floating-point stack over/underflow";
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        what = "FLOATING POINT ERROR";
        detail = "floating-point underflow";
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        what = "ILLEGAL INSTRUCTION";
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        what = "PAGE ERROR";
        if (excRec.NumberParameters >= 3)
        {
            // the first element contains a read/write flag
            // the second element contains the virtual address of the inaccessible data
            // the third element contains the underlying NTSTATUS code that caused the exception
            addr = &excRec.ExceptionInformation[1];
            uint64_t status = excRec.ExceptionInformation[2];
            snprintf(detailBuf, sizeof(detailBuf), "NTSTATUS=%lu", status);
            detail = detailBuf;
        }
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        what = "FLOATING POINT ERROR";
        detail = "integer divide by zero";
        break;
    case EXCEPTION_INT_OVERFLOW:
        what = "FLOATING POINT ERROR";
        detail = "integer overflow";
        break;
    case EXCEPTION_INVALID_DISPOSITION:
        what = "INVALID EXCEPTION HANDLER DISPOSITION";
        break;
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        what = "NONCONTINUABLE EXCEPTION";
        break;
    case EXCEPTION_PRIV_INSTRUCTION:
        what = "EXCEPTION_PRIV_INSTRUCTION";
        break;
    case EXCEPTION_SINGLE_STEP:
        what = "EXCEPTION_SINGLE_STEP";
        break;
    case EXCEPTION_STACK_OVERFLOW:
        // handled below
        break;
    default:
        what = "Unrecognized Exception";
        snprintf(detailBuf, sizeof(detailBuf), "exception code = %lu", excRec.ExceptionCode);
        detail = detailBuf;
        break;
    }

    // only print a back trace if the stack didn't overflow... (we would only make it worse)
    if (excRec.ExceptionCode != EXCEPTION_STACK_OVERFLOW)
    {
        char reason[256];
        formatReason(reason, what, detail, addr);
        abort(reason, true, true, (const uintptr_t*)&excRec.ExceptionAddress);
    }
    else
    {
        abort(REASON_PREFIX "SEGMENTATION FAULT (stack overflow)", false, true);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/**
 * Signal handler for Windows.
 * @param[in] sig    the signal number (ignored)
 */
static void onAbort(int sig)
{
    // ignored - only SIGABRT expected
    std::ignore = sig;

    char reason[256];
    formatReason(reason, "std::abort()", nullptr, nullptr);
    abort(reason, true, true);
}

#else  // !_WIN32

/// for systems supporting it, statically reserve it as an actual stack
static std::array<uint8_t, s_ALT_STACK_SIZE> s_ALT_STACK;

/**
 * Signal handler implementation for Linux.
 * @param[in] sig       the signal number
 * @param[in] info      additional information
 * @param[in] ctx       signal context
 */
[[noreturn]] static void signalHandler(int sig, siginfo_t* info, void* ctx)
{
    char buf[64];
    const char* what = "";
    const char* detail = nullptr;
    const uintptr_t* addr = nullptr;

    // determine were we got called from
    const uintptr_t* faultAddr = nullptr;
    auto* context = (const ucontext_t*)ctx;
    if (context)
    {
        static_assert(sizeof(greg_t) == sizeof(uintptr_t), "something is odd here...");
        faultAddr = (const uintptr_t*)&context->uc_mcontext.gregs[REG_RIP];
    }

    switch (sig)
    {
    case SIGABRT:
    {
        what = "std::abort()";
        break;
    }
    case SIGSEGV:
    {
        what = "SEGMENTATION FAULT";
        switch (info->si_code)
        {
        case SEGV_MAPERR:
            detail = "address not mapped to object";
            // may be a stack overflow...
            if (context)
            {
                // Let's try to distinguish the usual "segmentation fault" from a
                // "stack overflow": Check if the address causing the fault is "slightly"
                // past the end of the stack.
                auto stackPtr = (uintptr_t)context->uc_mcontext.gregs[REG_RSP];
                auto stackAddr = (uintptr_t)info->si_addr;
                constexpr auto rangeLimit = 1024;
                if (stackAddr < stackPtr && stackPtr - stackAddr < rangeLimit)
                    detail = "stack overflow";
            }
            break;
        case SEGV_ACCERR:
            detail = "invalid permissions for mapped object";
            break;
        case SEGV_BNDERR:
            detail = "failed address bound checks";
            break;
        case SEGV_PKUERR:
            detail = "access was denied by memory protection keys";
            break;
        default:
            break;
        }
        addr = reinterpret_cast<uintptr_t*>(&info->si_addr);
        break;
    }
    case SIGBUS:
    {
        what = "BUS ERROR";
        switch (info->si_code)
        {
        case BUS_ADRALN:
            detail = "invalid address alignment";
            break;
        case BUS_ADRERR:
            detail = "nonexistent physical address";
            break;
        case BUS_OBJERR:
            detail = "object-specific hardware error";
            break;
        case BUS_MCEERR_AR:
            detail = "hardware memory error consumed on a machine check";
            break;
        case BUS_MCEERR_AO:
            detail = "hardware memory error detected in process but not consumed";
            break;
        default:
            break;
        }
        addr = reinterpret_cast<uintptr_t*>(&info->si_addr);
        break;
    }
    case SIGILL:
    {
        what = "ILLEGAL INSTRUCTION";
        switch (info->si_code)
        {
        case ILL_ILLOPC:
            detail = "illegal opcode";
            break;
        case ILL_ILLOPN:
            detail = "illegal operand";
            break;
        case ILL_ILLADR:
            detail = "illegal addressing mode";
            break;
        case ILL_ILLTRP:
            detail = "illegal trap";
            break;
        case ILL_PRVOPC:
            detail = "privileged opcode";
            break;
        case ILL_PRVREG:
            detail = "privileged register";
            break;
        case ILL_COPROC:
            detail = "coprocessor error";
            break;
        case ILL_BADSTK:
            detail = "internal stack error";
            break;
        default:
            break;
        }
        addr = reinterpret_cast<uintptr_t*>(&info->si_addr);
        break;
    }
    case SIGFPE:
    {
        what = "FLOATING POINT ERROR";
        switch (info->si_code)
        {
        case FPE_INTDIV:
            detail = "integer divide by zero";
            break;
        case FPE_INTOVF:
            detail = "integer overflow";
            break;
        case FPE_FLTDIV:
            detail = "floating-point divide by zero";
            break;
        case FPE_FLTOVF:
            detail = "floating-point overflow";
            break;
        case FPE_FLTUND:
            detail = "floating-point underflow";
            break;
        case FPE_FLTRES:
            detail = "floating-point inexact result";
            break;
        case FPE_FLTINV:
            detail = "floating-point invalid operation";
            break;
        case FPE_FLTSUB:
            detail = "subscript out of range";
            break;
        default:
            break;
        }
        addr = reinterpret_cast<uintptr_t*>(&info->si_addr);
        break;
    }
    default:
    {
        // should not happen, but let's handle it
        snprintf(buf, sizeof(buf), "SIGNAL %d", sig);
        what = buf;
        break;
    }
    }

    char reason[256];
    formatReason(reason, what, detail, addr);
    abort(reason, true, true, faultAddr);
}
#endif // _WIN32

/// What to do when std::terminate is called
static void onTerminate()
{
    constexpr bool stackTrace = true;
    constexpr bool inSignalHandler = false;

    auto currentException = std::current_exception();
    if (currentException)
    {
        const char* const what = "std::terminate()";
        char detail[128];
        // if there is a current exception, re-throw it to handle it's type properly
        try
        {
            std::rethrow_exception(currentException);
        }
        // extract error codes from system_error
        catch (const std::system_error& exc)
        {
            // indicate the exception's type
            const std::error_code& err = exc.code();
            snprintf(detail, sizeof(detail), "std::system_error: \"%s\" (%s:%d)",
                     err.message().c_str(), err.category().name(), err.value());
        }
        // catch-all for all standard exceptions
        catch (const std::exception& exc)
        {
            // demangle the exception class name
            char className[128]{};
            demangle(typeid(exc).name(), className, sizeof(className), inSignalHandler);

            // format the exception's type and error message
            snprintf(detail, sizeof(detail), "%s: \"%s\"", className, exc.what());
        }
        // handle strings (should not be used, but who knows...)
        catch (const char* err)
        {
            if (!err)
                err = "<null>";

            // indicate the exception's type
            snprintf(detail, sizeof(detail), "exception (const char*): \"%s\"", err);
        }
        // anything else
        catch (...)
        {
            strncpy(detail, "unknown exception", sizeof(detail));
        }

        char reason[256];
        formatReason(reason, what, detail, nullptr);
        abort(reason, stackTrace, inSignalHandler);
    }
    else
    {
#ifdef _WIN32
        /*
         * Overwriting libstdc++'s ABI functions doesn't work here due to differences in shared
         * library loading and symbol resolution.
         * Therefore, check if we were called from __cxa_pure_*.
         */
        struct Function
        {
            const char* symbolName;
            uintptr_t symbolAddress;
            const char* abortMessage;
        };
        std::array<Function, 2> possibleCallers{
            Function{ "__cxa_pure_virtual", 0, "PURE VIRTUAL FUNCTION CALL" },
            Function{ "__cxa_deleted_virtual", 0, "DELETED VIRTUAL FUNCTION CALL" },
        };

        // only check up to 4 frames
        static constexpr DWORD lookBack = 4;
        void* stackFrames[lookBack];
        WORD numberOfFrames = RtlCaptureStackBackTrace(0, lookBack, stackFrames, NULL);
        // expect the library to be loaded - if not, it can't call us, right?
        HMODULE libstdcpp = GetModuleHandle("libstdc++-6.dll");
        if (libstdcpp)
        {
            for (auto& func : possibleCallers)
            {
                auto ptr = GetProcAddress(libstdcpp, func.symbolName);
                func.symbolAddress = (uintptr_t)ptr;
            }

            for (WORD i = 0; i < numberOfFrames; ++i)
            {
                const auto curFunc = (uintptr_t)stackFrames[i];
                for (const auto& func : possibleCallers)
                {
                    // check if the caller is identical or "shortly after" the function we're
                    // looking for
                    if (curFunc >= func.symbolAddress && curFunc - func.symbolAddress <= 64)
                    {
                        char reason[256];
                        formatReason(reason, func.abortMessage, nullptr, &curFunc);
                        abort(reason, stackTrace, inSignalHandler, &curFunc);
                        // note: not reached...
                        return;
                    }
                }
            }
        }
#endif

        // fallback...
        char reason[256];
        formatReason(reason, "std::terminate()", nullptr, nullptr);
        abort(reason, true, false);
    }
}

static bool s_handlersRegistered = false;

// Register signal and std::terminate handlers
HandlerSetup::HandlerSetup()
{
    // allow to disable the handlers, e.g. for debugging
    const char* opt = getenv("OOOPSI_DISABLE_HANDLERS"); // flawfinder: ignore
    if (opt && strcmp(opt, "1") == 0)
        return;

    if (s_handlersRegistered)
        return;
    else
        s_handlersRegistered = true;

    {
        // catch std::terminate
        std::set_terminate(onTerminate);
    }

#ifdef _WIN32
    {
        // catch Windows-exceptions
        SetUnhandledExceptionFilter(onWindowsException);
        // catch std::abort
        signal(SIGABRT, onAbort);
        // note: don't catch SIGSEGV - the exception filter will be called, which has more infos
    }
#else
    // error handler
    auto err = [](const char* what, int param) {
        char messageBuffer[256];
        snprintf(messageBuffer, sizeof(messageBuffer), "%s(%d) failed: %s", what, param,
                 strerror(errno));
        abort(messageBuffer, true, false);
    };

    // use an alternate stack in case we have a stack overflow!
    {
        stack_t altStack;
        altStack.ss_flags = 0;
        static_assert(s_ALT_STACK_SIZE >= MINSIGSTKSZ,
                      "Not fulfilling the system's minimum requirement!");
        altStack.ss_size = s_ALT_STACK.size();
        altStack.ss_sp = s_ALT_STACK.data();
        if (sigaltstack(&altStack, nullptr) != 0)
            err("sigaltstack", s_ALT_STACK_SIZE);
    }

    // catch fatal signals
    for (int sig : { SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE })
    {
        // prepare the signal structure
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_ONSTACK | SA_SIGINFO;
        act.sa_sigaction = signalHandler;

        if (sigaction(sig, &act, nullptr) != 0)
            err("sigaction", sig);
    }
#endif
}

HandlerSetup::~HandlerSetup()
{
    // skipping unregistering - not worth the hassle
}

/// static instance for RAII-style setup
/// (not working when linking statically though...)
static HandlerSetup s_setup;

} // namespace ooopsi
