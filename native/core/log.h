// Append-only log at %LOCALAPPDATA%\Atrium\logs\core.log, written by a thread of its own.
#pragma once

namespace atrium {

void Log(const wchar_t* format, ...);
void ShutdownLog();

} // namespace atrium
