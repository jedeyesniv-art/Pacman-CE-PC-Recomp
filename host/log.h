#pragma once

// Host log: every line goes to stderr and to the log file opened by LogInit.
void LogInit(const char* path);
void LogPrint(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogFlush();

// Number of lines logged so far; used to notice when the guest has gone quiet.
unsigned long long LogLineCount();
