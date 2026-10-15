//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once
#include <stdint.h>
#include <deque>

#include "../xemu-notifications.h"

class NotificationManager
{
private:
    std::deque<const char *> m_notification_queue;
    std::deque<const char *> m_error_queue;

    const int kNotificationDuration = 4000;
    uint32_t m_notification_end_time;
    const char *m_msg;
    bool m_active;

public:
    NotificationManager();
    void QueueNotification(const char *msg);
    void QueueError(const char *msg);
    void Draw();

private:
    void DrawNotification(float t, const char *msg);
};

extern NotificationManager notification_manager;

//
// Popups (the connection check panel, the notices and the notifications)
// go in one column in a top corner, each under the one before, so none
// hides another. The column is on the left, or on the right when the
// performance overlay is in the top left.
//
void PopupStackReset();  // once a frame, before the first popup
void PopupStackPlace();  // before a popup's Begin: puts it in the next slot
void PopupStackTake();   // inside the popup, before End: takes the slot
