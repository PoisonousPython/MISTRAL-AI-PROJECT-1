#pragma once

namespace tgov {

void Logf(const char* fmt, ...);
void LogOpen(const char* log_path);
void LogClose();

}  // namespace tgov
