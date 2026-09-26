#pragma once
#include <windows.h>
#include "log.h"
namespace FaultGuard {
inline const char* stage="initialization";
inline int Report(EXCEPTION_POINTERS* exception){
    const auto* record=exception->ExceptionRecord;
    logfile::Line("Runtime fault: stage=%s code=0x%08lX address=%p",stage,record->ExceptionCode,record->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
}
