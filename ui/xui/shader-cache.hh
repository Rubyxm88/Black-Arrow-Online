//
// xemu User Interface: Shader Cache window
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

class ShaderCacheWindow
{
public:
    bool is_open;

private:
    bool   m_offer_checked;  // looked once whether to offer a precache
    bool   m_scan_requested;
    bool   m_confirm_clear;  // Clear Cache was clicked once
    double m_cleared_time;   // when the cache was cleared, to say so

public:
    ShaderCacheWindow();
    void Open();
    void Draw();

private:
    void CheckOffer();
};

extern ShaderCacheWindow shader_cache_window;
