//
// xemu User Interface: Troubleshoot > Make a Full Report on a Mac and on Linux
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

// Starts the Black Arrow Report setup.sh installed; a notice says where its
// file is when it is done
void FullReportStart();
bool FullReportRunning();
// Each frame: shows the notice of a report that is done
void FullReportPoll();
