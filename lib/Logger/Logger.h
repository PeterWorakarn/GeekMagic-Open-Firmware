// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

static constexpr size_t LOG_BUFFER_MAX_ENTRIES = 50;
static constexpr size_t LOG_ENTRY_MAX_LEN = 128;

class Logger {
   public:
    static void log(LogLevel level, const char* message, const char* className = nullptr);
    static void debug(const char* message, const char* className = nullptr);
    static void info(const char* message, const char* className = nullptr);
    static void warn(const char* message, const char* className = nullptr);
    static void error(const char* message, const char* className = nullptr);

    static String getLogsAsString();
    static size_t getLogCount();
    static const char* getLogEntry(size_t index);
    static void clearLogs();

   private:
    static void printTime();
    static const char* levelToString(LogLevel level);
    static void addToBuffer(const char* entry);

    static char _logBuffer[LOG_BUFFER_MAX_ENTRIES][LOG_ENTRY_MAX_LEN];
    static size_t _head;
    static size_t _count;
};

#endif  // LOGGER_H
