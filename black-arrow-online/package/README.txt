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
   Need a code? Menu: XB Online > Link Code.
   No account yet? See NEW ACCOUNT below first.
4. In the dashboard: Settings > Network, set the top option to XB Online.
5. Menu: XB Online > Black Arrow - Install Game Updates, and follow the
   prompts.
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
GAMES
  Play <game>                   Start a game from the "games" folder.
                                No games yet: opens the games folder.
  Black Arrow - Install Game Updates
                                Black Arrow title update and map packs.
DASHBOARDS
  XBL Dashboard                 Sign in, link or fix your Xbox Live account.
  Original Xbox Dashboard       The standard Xbox dashboard (saves, settings).
XB LIVE ACCOUNT
  Account: <gamertag>           The account on this console. Opens your
                                xb.live dashboard in the browser.
  Create an Account             Opens xb.live/xb-online (see NEW ACCOUNT).
  Link Code                     Opens xb.live/link in your browser.
HELP
  Connection Check              Runs the XB Online check (see below).
  Record Diagnostics Log        Records a log while you play (see LAG).
  Mark Lag Spike (F9)           Marks the moment in the log you are recording.
  Open Last Freeze Report       Opens the newest freeze report (see FREEZES).
  Open Logs Folder              Opens system\logs.
  Shader Cache                  See SHADERS below.

On a controller, press the Guide/Home button for the same options.

The XBL Dashboard is installed on the console's hard disk (clean start), so
it can update itself when it offers an update. If you brought your own hard
disk it runs from the disc image instead and stays at the bundled version.


THE RSO SETTINGS MENU (next to XB Online)
-----------------------------------------
Every switch here can be turned back off if something looks wrong.
SHADERS
  Shader Cache...               How many shaders are ready, build or clear them.
  Save shaders to disk          Keep built shaders for the next start.
  Build shaders in the background
                                A new effect builds without freezing the game;
                                it may be missing for a frame or two instead.
  Precache shaders automatically
                                See SHADERS below.
  Precache this PC's own shaders
                                Also rebuild shaders this PC saw before (after
                                a driver update or a resolution change).
GRAPHICS
  Renderer                      OpenGL (recommended) or Vulkan. Needs a restart.
                                Shader caching works with OpenGL only.
  Resolution                    1x (fastest) to 4x. 2x is the default.
  Skip render target copy-back  Speed hack. Turn off if the game looks wrong.
  Vertical sync                 Turn off only if you know you need to.
DIAGNOSTICS
  Record Diagnostics Log, Open Logs Folder (see LAG).
  Save a report when the game freezes
                                On by default (see FREEZES).
  Open Last Freeze Report       See FREEZES.


SHADERS
-------
The first time an effect appears (a map, night vision, thermal, a menu),
your graphics card has to build a shader for it, which used to make the
game hitch. Black Arrow Online ships a list of the game's shaders: a few
seconds after starting, it builds the ones not ready yet for your graphics
card and resolution in the background. A small notice in the top left shows
the progress, then "Shaders ready". This takes a few seconds and only
happens again after a graphics driver update or a resolution change.
To turn it off: RSO Settings > Precache shaders automatically.


LAG
---
If the game stutters or lags: XB Online > Record Diagnostics Log, play as
usual, and press F9 right after each lag spike. Stop the log from the same
menu (or just close the emulator). The log is in system\logs (Open Logs
Folder); send it to whoever shared the package. It holds your PC's
specs, frame times and what caused each slow frame, not your account.


FREEZES
-------
You don't have to turn anything on. While you play, the emulator keeps the
last minute of the diagnostics log in memory and writes nothing. If the
game freezes (no new picture for 10 seconds while the emulator runs), it
saves a short freeze report in system\logs and shows "The game stopped
responding" in the top left. Open it with XB Online > Help > Open Last
Freeze Report and send it to whoever shared the package. Only the newest 5
reports are kept. To turn it off: RSO Settings > Save a report when the
game freezes.


NEW ACCOUNT
-----------
Xbox Live accounts for XB Online are made on xb.live.
1. Menu: XB Online > Create an Account. It opens xb.live/xb-online in your
   browser; create the account there.
2. Then go on with step 3 of FIRST TIME: sign in to the XBL Dashboard and
   link the account (Link Code gives you the code).
Your gamertag then shows in the XB Online menu under XB LIVE ACCOUNT.

UPDATES
-------
When a newer build is out, Black Arrow Online offers it when you start.
An update replaces the launcher, the emulator and the shader list only. Your console, Xbox
Live sign-in, saves, settings and games are not touched. The previous
emulator is kept as system\xemu.previous.exe.
To turn the check off, create an empty file system\no-update-check.txt.
Builds and notes: https://github.com/Rubyxm88/Black-Arrow-Online/releases
The downloads there are updates for this folder, not the full package.


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


XB ONLINE CHECK
---------------
A few seconds after starting, Black Arrow Online quietly checks what Xbox
Live needs: the console network, the XB Online server, this PC's clock,
the game disc, the game update and the Xbox Live account. When everything
is fine, "XB Online Connected" with a green checkmark shows for a moment.
When something is wrong, a panel in the top left corner says what and how
to fix it. Run it any time from XB Online > Connection Check.
The results are also written to system\xemu.log.


TROUBLE
-------
Setup window says "Add content pack" instead of "Start clean": this folder
  is an update download, not the full package. Get the full package from
  where Black Arrow Online is shared and start from that.
"Xbox Live not found" or sign-in fails in the game:
  - Look at the XB Online check panel (XB Online > Connection Check) and
    fix whatever it marks FIX or WARN first.
  - VPN: turn it off completely (also auto-connect and kill switch) and
    restart Black Arrow Online. A VPN answers the Xbox Live lookups itself,
    so the console never reaches XB Online. A sign: Settings > Network in
    the dashboard shows an address that is not this PC's normal one
    (for example 10.14.0.2).
  - "PC clock": Xbox Live sign-in fails when the Windows clock or time
    zone is wrong. Settings > Time: set time and time zone automatically.
  - "Game update": XB Online > Black Arrow - Install Game Updates.
  - "XB Online server": a firewall, antivirus, VPN or router DNS setting is
    in the way. Allow xemu through the firewall, or try another network.
  - Close any other xemu or emulator and any game that is still running,
    then start Black Arrow Online again. Another program on UDP port 3074
    switched the console's network off in older builds.
  - Sign in with the account the XBL Dashboard added for XB Online. An
    account that is not linked to xb.live does not work until you link it
    (FIRST TIME, step 3).
  - Still failing: send system\xemu.log to whoever shared the package.
Game looks wrong: RSO Settings, turn off "Skip render target copy-back".
  Effects missing or flickering: turn off "Build shaders in the background".
Slow: RSO Settings > Resolution, choose 1x.
Stutter on a 144 Hz (or other high refresh) monitor: the game runs at 30
  frames, which divides evenly into 60, 120 or 240 Hz but not 144 or 165.
  Set the monitor to 120 Hz (or 60 Hz) in Windows display settings.
Hosting a game:
  Players on your own network (another Xbox or PC) can join as it is. Allow
  xemu through Windows Firewall when Windows asks.
  Players over the internet can only join if your router forwards UDP port
  3074 to this PC.
  Only one copy of Black Arrow Online per PC can host.
