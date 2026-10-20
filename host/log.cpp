#define _CRT_SECURE_NO_WARNINGS // fopen
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

static std::mutex g_logMutex;
static FILE* g_logFile;

void LogInit(const char* path)
{
    g_logFile = std::fopen(path, "w");
    if (g_logFile == nullptr)
        std::fprintf(stderr, "warning: could not open log file %s\n", path);
}

void LogPrint(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    std::lock_guard lock(g_logMutex);
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    if (g_logFile != nullptr)
    {
        std::fputs(line, g_logFile);
        std::fputc('\n', g_logFile);
    }
}

void LogFlush()
{
    std::lock_guard lock(g_logMutex);
    std::fflush(stderr);
    if (g_logFile != nullptr)
        std::fflush(g_logFile);
}
