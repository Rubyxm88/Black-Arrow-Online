BLACK ARROW ONLINE
==================
Rainbow Six 3: Black Arrow on XB Online, ready to run.


FIRST TIME
----------
1. Copy your Black Arrow disc image (.iso) into the "games" folder.
2. Double-click "Black Arrow Online". A setup window appears once:
     Start clean                 a brand new console, or
     Use my existing xemu files  copies these into this folder:
                                   xbox_hdd.qcow2  your Xbox hard disk (required)
                                   eeprom.bin      its console identity (required)
                                   BIOS / MCPX     optional, included ones are used
3. The XBL Dashboard opens. Sign in with your xb.live account.
   Need a code? Menu: XB Online > Get a Link Code.
4. In the dashboard: Settings > Network, set the top option to XB Online.
5. Menu: XB Online > Install Game Update + Maps, and follow the prompts.
   (Skip this if your own hard disk already has them.)
6. Menu: XB Online > Play Rainbow Six 3 Black Arrow.


EVERY TIME AFTER THAT
---------------------
Double-click "Black Arrow Online". There is no setup window any more: it
goes straight to the game. If the games folder is empty it opens the XBL
Dashboard instead. Dashboards, account setup and the update installer stay
available in the XB Online menu.


THE XB ONLINE MENU (top of the window)
--------------------------------------
Play <game>                     Start a game from the "games" folder.
Add or Load Another Game        Pick a disc image stored somewhere else.
XBL Dashboard (Account Setup)   Sign in, link or fix your Xbox Live account.
Original Xbox Dashboard         The standard Xbox dashboard (saves, settings).
Install Game Update + Maps      Black Arrow title update and map packs.
Get a Link Code                 Opens xb.live/link in your browser.

On a controller, press the Guide/Home button for the same options.

The XBL Dashboard is installed on the console's hard disk (clean start), so
it can update itself when it offers an update. If you brought your own hard
disk it runs from the disc image instead and stays at the bundled version.


CONTROLLER
----------
Plug in a controller before starting and it is used automatically.
To change it: Machine > Settings > Input.


STARTING OVER
-------------
"system\Start over.bat" removes the console in this folder. The next start
asks again whether you want a clean start or your own files.


DO NOT SHARE
------------
After setup, system\eeprom.bin and system\xbox_hdd.qcow2 belong to your
console and your account. Never send them to anyone. Share the original zip.


TROUBLE
-------
Game looks wrong: Machine > Settings > Display, turn off
  "Skip render target copy-back (speed hack)".
Slow: View > Int. Resolution Scale, choose 1x.
Hosting a game:
  Players on your own network (another Xbox or PC) can join as it is. Allow
  xemu through Windows Firewall when Windows asks.
  Players over the internet can only join if your router forwards UDP port
  3074 to this PC.
  Only one copy of Black Arrow Online per PC can host.
