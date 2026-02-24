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

#include <ctime>
#include <cstring>
#include "Logger.h"

char Logger::_logBuffer[LOG_BUFFER_MAX_ENTRIES][LOG_ENTRY_MAX_LEN];
size_t Logger::_head = 0;
size_t Logger::_count = 0;

/**
 * @brief Logs a message with a specified log level
 *
 * @param level The severity level of the log message
 * @param message The message to be logged
 * @param className optional class name for context
 */
void Logger::log(LogLevel level, const char* message, const char* className) {
    char entry[LOG_ENTRY_MAX_LEN];
    char timeBuf[12];
    std::time_t t = std::time(nullptr);
    std::tm* now = std::localtime(&t);
    snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d]", now->tm_hour, now->tm_min, now->tm_sec);

    const char* classStr = (className != nullptr && className[0] != '\0') ? className : "Global";

    snprintf(entry, sizeof(entry), "%s(%s)::%s: %s", timeBuf, levelToString(level), classStr, message);

    addToBuffer(entry);

    Serial.println(entry);
}

/**
 * @brief Logs a debug message
 *
 * @param message The debug message to log
 * @param className optional class name for context
 */
void Logger::debug(const char* message, const char* className) { log(LOG_DEBUG, message, className); }

/**
 * @brief Logs an info message
 *
 * @param message The info message to log
 * @param className optional class name for context
 */
void Logger::info(const char* message, const char* className) { log(LOG_INFO, message, className); }

/**
 * @brief Logs a warning message
 *
 * @param message The warning message to log
 * @param className optional class name for context
 */
void Logger::warn(const char* message, const char* className) { log(LOG_WARN, message, className); }

/**
 * @brief Logs an error message
 *
 * @param message The error message to log
 * @param className optional class name for context
 */
void Logger::error(const char* message, const char* className) { log(LOG_ERROR, message, className); }

/**
 * @brief Print the current local time in [HH:MM:SS]
 */
void Logger::printTime() {
    char buffer[20];
    std::time_t t = std::time(nullptr);
    std::tm* now = std::localtime(&t);
    snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d]", now->tm_hour, now->tm_min, now->tm_sec);
    Serial.print(buffer);
}

/**
 * @brief Add a log entry to the circular buffer
 *
 * @param entry The log entry string
 */
void Logger::addToBuffer(const char* entry) {
    strncpy(_logBuffer[_head], entry, LOG_ENTRY_MAX_LEN - 1);
    _logBuffer[_head][LOG_ENTRY_MAX_LEN - 1] = '\0';

    _head = (_head + 1) % LOG_BUFFER_MAX_ENTRIES;
    if (_count < LOG_BUFFER_MAX_ENTRIES) {
        _count++;
    }
}

/**
 * @brief Get all logs as a single string separated by newlines
 *
 * @return String containing all log entries
 */
String Logger::getLogsAsString() {
    String result;
    result.reserve(_count * 64);
    for (size_t i = 0; i < _count; i++) {
        result += getLogEntry(i);
        result += '\n';
    }
    return result;
}

/**
 * @brief Get log entry count
 *
 * @return Number of log entries in buffer
 */
size_t Logger::getLogCount() { return _count; }

/**
 * @brief Get a specific log entry by index (oldest first)
 *
 * @param index Index of the log entry
 * @return Pointer to the log entry string, or nullptr if out of range
 */
const char* Logger::getLogEntry(size_t index) {
    if (index >= _count) {
        return nullptr;
    }
    size_t actualIndex;
    if (_count < LOG_BUFFER_MAX_ENTRIES) {
        actualIndex = index;
    } else {
        actualIndex = (_head + index) % LOG_BUFFER_MAX_ENTRIES;
    }
    return _logBuffer[actualIndex];
}

/**
 * @brief Clear all logs from the buffer
 */
void Logger::clearLogs() {
    _head = 0;
    _count = 0;
}

/**
 * @brief Converts a LogLevel enum value to its corresponding string representation
 *
 * @param level The log level
 * @return A constant character pointer to the string representation of the log level
 *         Returns "UNKNOWN" if the log level is not recognized
 */
const char* Logger::levelToString(LogLevel level) {
    switch (level) {
        case LOG_DEBUG:
            return "DEBUG";
        case LOG_INFO:
            return "INFO";
        case LOG_WARN:
            return "WARN";
        case LOG_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
