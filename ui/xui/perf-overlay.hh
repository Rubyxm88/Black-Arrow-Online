//
// xemu User Interface: performance overlay
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

// Draws the overlay when View > Performance Overlay has it on
void DrawPerfOverlay();

// Where the overlay ends at the top right this frame, so a notice there can
// go under it; 0 when it is not in that corner
float PerfOverlayTopRightBottom();
