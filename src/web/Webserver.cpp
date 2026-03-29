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

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <functional>
#include <Logger.h>
#include <cstring>
#include <cstdlib>

#include "web/Webserver.h"

/**
 * @brief Construct a new Webserver object
 * @param port Port number to listen on
 *
 * @return void
 */
Webserver::~Webserver()  // NOLINT(modernize-use-equals-default)
{
    for (char* p : _staticAllocFallbackPtrs) {
        if (p != nullptr) {
            free(p);
        }
    }
    _staticAllocFallbackPtrs.clear();

    for (char* pool : _staticAllocPools) {
        if (pool != nullptr) {
            free(pool);
        }
    }
    _staticAllocPools.clear();
}

Webserver::Webserver(uint16_t port) : _server(port) {}

/**
 * @brief Initializes the LittleFS filesystem
 * @param formatIfFailed Whether to format the filesystem if mounting fails
 *
 * @return true if filesystem is mounted false otherwise
 */
auto Webserver::beginFS(bool formatIfFailed) -> bool {
    if (LittleFS.begin()) {
        return true;
    };

    if (formatIfFailed) {
        return LittleFS.begin();
    };

    return false;
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
/**
 * @brief Starts the webserver
 *
 * @return void
 */
void Webserver::begin() {
    Logger::info("Starting webserver", "Webserver");
    _server.begin();
}
// NOLINTEND(readability-convert-member-functions-to-static)

/**
 * @brief Handles incoming client requests
 *
 * @return void
 */
void Webserver::handleClient() { _server.handleClient(); }

/**
 * @brief Register a handler for a route
 * @param uri The URI path to handle
 * @param method The HTTP method to handle
 * @param handler The function to call when the route is accessed
 *
 * @return void
 */
void Webserver::on(const String& uri, HTTPMethod method, std::function<void()> handler) {
    _server.on(uri.c_str(), method, [handler]() { handler(); });
}

/**
 * @brief Register a generic handler (all methods)
 * @param uri The URI path to handle
 * @param handler The function to call when the route is accessed
 *
 * @return void
 */
void Webserver::on(const String& uri, std::function<void()> handler) {
    _server.on(uri.c_str(), [handler]() { handler(); });
}

/**
 * @brief Serve a static file from LittleFS using C-strings. If a .gz variant exists, serve it with gzip encoding
 * @param uriC The URL path
 * @param pathC The filesystem path
 * @param contentTypeC The content type to use. If nullptr or empty string, it will be derived from the file extension
 * @param cacheSeconds The number of seconds to cache the file (0 = no-cache)
 * @param tryGzip Whether to try serving a .gz variant if it exists
 *
 * @return void
 */
void Webserver::serveStaticC(const char* uriC, const char* pathC, const char* contentTypeC, int cacheSeconds,
                             bool tryGzip) {
    _server.on(uriC, HTTP_GET, [this, pathC, contentTypeC, cacheSeconds, tryGzip, uriC]() {
        char servePath[256];
        char gzPath[260];
        const char* chosenPath = pathC;
        const char* contentTypeStr = contentTypeC;

        // compute gz path if requested
        if (tryGzip) {
            size_t l = strnlen(pathC, sizeof(servePath) - 1);
            if (l + 4 < sizeof(gzPath)) {
                memcpy(gzPath, pathC, l);
                gzPath[l] = '\0';
                strcat(gzPath, ".gz");
                if (LittleFS.exists(gzPath)) {
                    chosenPath = gzPath;
                }
            }
        }

        if (!LittleFS.exists(chosenPath)) {
            char msg[320];
            snprintf(msg, sizeof(msg), "File not found: %s", chosenPath);
            Logger::error(msg, "Webserver");
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");

            return;
        }

        File f = LittleFS.open(chosenPath, "r");
        if (!f) {
            char msg[320];
            snprintf(msg, sizeof(msg), "Failed to open file: %s", chosenPath);
            Logger::error(msg, "Webserver");
            _server.send(HTTP_CODE_INTERNAL_ERROR, "text/plain", "Open failed");

            return;
        }

        size_t size = f.size();

        char ctBuf[64] = {0};
        if (contentTypeStr != nullptr && contentTypeStr[0] != '\0') {
            strncpy(ctBuf, contentTypeStr, sizeof(ctBuf) - 1);
        } else {
            String guessed = guessContentType(String(chosenPath));
            strncpy(ctBuf, guessed.c_str(), sizeof(ctBuf) - 1);
        }

        if (cacheSeconds > 0) {
            char headerVal[64];
            snprintf(headerVal, sizeof(headerVal), "public, max-age=%d", cacheSeconds);
            _server.sendHeader("Cache-Control", String(headerVal));
        } else {
            _server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        }

        // Content-Encoding header when gz served
        if (chosenPath == gzPath) {
            _server.sendHeader("Content-Encoding", "gzip");
        }

        _server.setContentLength(size);
        _server.streamFile(f, String(ctBuf));
        f.close();

        char infoMsg[320];
        snprintf(infoMsg, sizeof(infoMsg), "Served %s for URI: %s", chosenPath, uriC);
        Logger::info(infoMsg, "Webserver");
    });
}

/**
 * @brief Register all files in a LittleFS directory as static routes
 * @param fsDir The LittleFS directory path
 * @param uriPrefix The URI prefix to use
 * @param contentType The content type to use for all files
 *
 * @return void
 */
void Webserver::registerStaticDir(  // NOLINT(readability-convert-member-functions-to-static)
    const String& fsDir, const String& uriPrefix, const String& contentType) {
    String dirPath = fsDir;
    if (dirPath.endsWith("/") && dirPath.length() > 1) {
        dirPath = dirPath.substring(0, dirPath.length() - 1);
    }

    String prefix = uriPrefix;
    if (prefix.endsWith("/") && prefix.length() > 1) {
        prefix = prefix.substring(0, prefix.length() - 1);
    }

    if (!LittleFS.exists(dirPath)) {
        Logger::warn((String("Static dir not found: ") + dirPath).c_str(), "Webserver");
        return;
    }

    _server.serveStatic(prefix.c_str(), LittleFS, dirPath.c_str(), "max-age=86400");

    String info = String("Registered static dir: ") + prefix + " -> " + dirPath;
    if (!contentType.isEmpty()) {
        info += String(" (ct=") + contentType + ")";
    }
    Logger::info(info.c_str(), "Webserver");
}

/**
 * @brief Register a generic static fallback route using onNotFound
 *
 * Serves GET requests from fsBasePath + request URI. API routes are excluded
 * Can optionally exclude '/' to keep an explicit root route
 */
void Webserver::registerGenericStaticFallback(  // NOLINT(readability-convert-member-functions-to-static)
    const String& fsBasePath, bool excludeRoot) {
    String basePath = fsBasePath;
    if (basePath.endsWith("/") && basePath.length() > 1) {
        basePath = basePath.substring(0, basePath.length() - 1);
    }

    _server.onNotFound([this, basePath, excludeRoot]() {
        if (_server.method() == HTTP_OPTIONS) {
            _server.sendHeader("Access-Control-Allow-Origin", "*");
            _server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            _server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
            _server.sendHeader("Access-Control-Max-Age", "3600");
            _server.send(HTTP_CODE_OK);
            return;
        }

        if (_server.method() != HTTP_GET) {
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");
            return;
        }

        const String uri = _server.uri();

        if (excludeRoot && uri == "/") {
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");
            return;
        }

        if (uri.startsWith("/api/")) {
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");
            return;
        }

        String fsPath = basePath + uri;
        String chosenPath = fsPath;

        if (LittleFS.exists(fsPath + ".gz")) {
            chosenPath = fsPath + ".gz";
        } else if (!LittleFS.exists(fsPath)) {
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");
            return;
        }

        File f = LittleFS.open(chosenPath, "r");
        if (!f) {
            _server.send(HTTP_CODE_INTERNAL_ERROR, "text/plain", "Open failed");
            return;
        }

        _server.sendHeader("Cache-Control", "public, max-age=86400");

        if (chosenPath.endsWith(".gz")) {
            _server.sendHeader("Content-Encoding", "gzip");
        }

        _server.setContentLength(f.size());
        _server.streamFile(f, guessContentType(fsPath));
        f.close();

        char infoMsg[320];
        snprintf(infoMsg, sizeof(infoMsg), "Served %s for URI: %s", chosenPath.c_str(), uri.c_str());
        Logger::info(infoMsg, "Webserver");
    });
}

/**
 * @brief Simple notFound handler registration
 * @param handler The function to call when a route is not found
 *
 * @return void
 */
void Webserver::onNotFound(std::function<void()> handler) {
    _server.onNotFound([handler]() { handler(); });
}

/**
 * @brief Expose underlying server where advanced config is needed
 *
 * @return reference to the underlying ESP8266WebServer
 */
auto Webserver::raw() -> ESP8266WebServer& { return _server; }

/**
 * @brief Guess the content type based on the file extension
 * @param path The file path
 *
 * @return The guessed content type
 */
auto Webserver::guessContentType(const String& path) -> String {
    if (path.endsWith(".html") || path.endsWith(".htm") || path.endsWith("/")) {
        return "text/html";
    }

    if (path.endsWith(".css")) {
        return "text/css";
    }

    if (path.endsWith(".js")) {
        return "application/javascript";
    }

    if (path.endsWith(".json")) {
        return "application/json";
    }

    if (path.endsWith(".png")) {
        return "image/png";
    }

    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) {
        return "image/jpeg";
    }

    if (path.endsWith(".gif")) {
        return "image/gif";
    }

    if (path.endsWith(".svg")) {
        return "image/svg+xml";
    }

    if (path.endsWith(".ico")) {
        return "image/x-icon";
    }

    if (path.endsWith(".txt")) {
        return "text/plain";
    }

    return "application/octet-stream";
}
