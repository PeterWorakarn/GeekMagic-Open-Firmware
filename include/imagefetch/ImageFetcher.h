// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef IMAGE_FETCHER_H
#define IMAGE_FETCHER_H

#include <Arduino.h>

class ImageFetcher {
   public:
    static constexpr const char* CACHE_PATH = "/cache/locket.jpg";
    static constexpr const char* CACHE_TMP_PATH = "/cache/locket.tmp";

    static void begin();
    static void loop();
    static bool fetchAndDisplayNow();

    static const String& lastStatus();
    static bool lastOk();

   private:
    static bool downloadToCache(const String& url);
    static bool decodeAndDraw();

    static unsigned long lastFetchMs_;
    static bool firstRunPending_;
    static String lastStatus_;
    static bool lastOk_;
};

#endif  // IMAGE_FETCHER_H
