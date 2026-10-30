//
// xemu User Interface: Black Arrow Online build numbers with sub builds
//
// A build is a whole number (26), or a sub build of it (26.1, 26.2...) that
// updates only some systems. Version files keep "build=26", which every older
// launcher and app reads as a whole number, and add "sub=1" for a sub build;
// its release tag is build-26.1. Inside the program one number holds both:
// build * 100 + sub (2601 for 26.1), so comparing two builds stays a plain
// comparison.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <string>
#include <stdlib.h>

#define BUILD_SUBS 100

// The whole build: 26 for 26 and 26.1
static inline int BuildWhole(int code)
{
    return code / BUILD_SUBS;
}

// From the build= and sub= values of a version file; 0 for a test build
static inline int BuildFromFields(const std::string &build,
                                  const std::string &sub)
{
    int whole = atoi(build.c_str());
    int part = atoi(sub.c_str());
    if (whole <= 0) {
        return 0;
    }
    if (part < 0 || part >= BUILD_SUBS) {
        part = 0;
    }
    return whole * BUILD_SUBS + part;
}

// From "26" or "26.1" (update-skip.txt, update-done.txt, a release tag
// without its "build-"); 0 when it is neither
static inline int BuildFromText(const std::string &text)
{
    size_t dot = text.find('.');
    std::string whole = text.substr(0, dot);
    std::string sub = dot == std::string::npos ? "" : text.substr(dot + 1);
    for (std::string *s : { &whole, &sub }) {
        while (!s->empty() && (s->back() == '\r' || s->back() == '\n' ||
                               s->back() == ' ')) {
            s->pop_back();
        }
        if (s->find_first_not_of("0123456789") != std::string::npos ||
            s->size() > 4) {
            return 0;
        }
    }
    if (dot != std::string::npos && sub.empty()) {
        return 0;
    }
    return BuildFromFields(whole, sub);
}

// "26" or "26.1"
static inline std::string BuildText(int code)
{
    std::string text = std::to_string(code / BUILD_SUBS);
    if (code % BUILD_SUBS) {
        text += "." + std::to_string(code % BUILD_SUBS);
    }
    return text;
}

// The release tag: build-26 or build-26.1
static inline std::string BuildTag(int code)
{
    return "build-" + BuildText(code);
}
