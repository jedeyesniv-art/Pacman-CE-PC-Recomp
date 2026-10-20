#pragma once

// Host log: every line goes to stderr and to the log file opened by LogInit.
void LogInit(const char* path);
void LogPrint(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogFlush();
