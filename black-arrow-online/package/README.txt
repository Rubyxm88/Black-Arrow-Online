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
   Need a code? Menu: XB Live > Link Code.
   No account yet? See NEW ACCOUNT below first.
4. In the dashboard: Settings > Network, set the top option to XB Online.
5. Menu: Play > Black Arrow - Install Game Updates, and follow the
   prompts.
   (Skip this if your own hard disk already has them.)
6. Menu: Play > Play Rainbow Six 3 Black Arrow.

All of this is also in the app: Help > Black Arrow Online Guide.


EVERY TIME AFTER THAT
---------------------
Double-click "Black Arrow Online". There is no setup window any more: it
goes straight to the game. If the games folder is empty it opens the XBL
Dashboard instead. Dashboards, account setup and the update installer stay
available in the Play and XB Live menus.


THE MENUS (top of the window)
-----------------------------
Each menu has one job. On a controller, press the Guide/Home button for the
same options.

MACHINE        the console itself
  Pause, Screenshot (F12), Snapshot, Settings..., Reset, Exit

PLAY           what is in the disc drive
  GAMES
    Play <game>               Start a game from the "games" folder.
                              No games yet: opens the games folder.
    Load Disc... / Eject Disc Any other disc image.
  GAME UPDATES
    Black Arrow - Install Game Updates
                              Black Arrow title update and map packs.
    Old Bow - Install Game Updates
                              Rainbow Six 3 title update and map packs
                              (see RAINBOW SIX 3 below).
  DASHBOARDS
    XBL Dashboard             Sign in, link or fix your Xbox Live account.
    Original Xbox Dashboard   The standard Xbox dashboard (saves, settings).

XB LIVE        your Xbox Live account and the connection
  ACCOUNT
    Account: <gamertag>       The account on this console. Opens your
                              xb.live dashboard in the browser.
    Create an Account         Opens xb.live/xb-online (see NEW ACCOUNT).
    Link Code                 Opens xb.live/link in your browser.
  CONNECTION
    Connection Check          Runs the XB Online check (see below).

SOUND          how the game sounds
  QUALITY
    Fast (recommended)        Mixes the game's sounds straight to your
                              speakers. Steadiest frame rate.
    Accurate                  Runs the Xbox's own sound effects processor.
                              Uses about a quarter of a CPU core more.
                              Stereo only.
  CHANNELS
    Stereo                    Two speakers or headphones (the default).
    5.1 Surround              Front, center, sub and rear speakers. Uses
                              Fast and sets the Xbox's audio to Dolby
                              Digital for you, so shots behind you come from
                              the rear speakers.
  The game reads the Xbox's audio setting when it starts. When a choice
  needs the game to restart (5.1 after the game started in Stereo, or
  Accurate after it started in 5.1), a Restart to Apply notice at the top
  right offers Restart game (it restarts the game, not the app). It goes
  away when you choose something that does not need the restart.
  Going back to a build before 24 sets sound to Stereo first: those builds
  close when the game starts with 5.1 on. If you copy an older build in by
  hand, set Channels to Stereo first.
  GAMEPLAY
    Louder movement           Footsteps and gear rattle are easier to hear.

GRAPHICS       how the game looks; every switch can be turned back off
  QUALITY
    Resolution                1x (fastest) to 4x. 2x is the default.
    Enhanced graphics         Antialiasing, sharpening, trilinear filtering,
                              anisotropic filtering and crisp menus.
  DISPLAY
    Fullscreen (F11), Vertical sync
    Window & Scaling          Aspect Ratio, Display Mode, UI Scale and
                              Filter Method.
  SHADERS
    Save shaders to disk      Keep built shaders for the next start.
    Build shaders in the background
                              A new effect builds without freezing the game;
                              it may be missing for a frame or two instead.
    Precache                  Precache shaders automatically (see SHADERS
                              below), and Precache this PC's own shaders.
  PERFORMANCE
    Improve render handling   Speed-up. Turn off if the game looks wrong.
    Performance Overlay       FPS, 1% low, frame times with a graph, CPU,
                              GPU, RAM and video memory over the game, with
                              style and corner customization.
    Renderer                  OpenGL (recommended) or Vulkan. Needs a restart.
                              Shader caching works with OpenGL only.

TROUBLESHOOT   when something goes wrong
  CONNECTION
    Connection Check          The same check as in XB Online.
  LAG & FREEZES
    Record Diagnostics Log    Records a log while you play (see LAG).
    Mark Lag Spike (F9)       Marks the moment in the log you are recording.
    Save a report when the game freezes
                              On by default (see FREEZES).
    Open Last Freeze Report   Opens the newest freeze report (see FREEZES).
  CACHES
    Shader Cache...           How many shaders are ready, build or clear them.
    Empty Game Cache...       See GAME CACHE below.
  FILES
    Open Logs Folder          Opens system\logs.
  Developer                   Monitor, audio and video debug windows.

HELP
  Black Arrow Online Guide... This README, inside the app, with buttons.
  Versions & Updates...       Install a newer or an older build (see UPDATES).
  About

The build you have shows at the right of the menu bar ("BUILD 15"). An amber
dot on it means a newer build is out; click it for Versions & Updates.

The XBL Dashboard is installed on the console's hard disk (clean start), so
it can update itself when it offers an update. If you brought your own hard
disk it runs from the disc image instead and stays at the bundled version.


SHADERS
-------
The first time an effect appears (a map, night vision, thermal, a menu),
your graphics card has to build a shader for it, which used to make the
game hitch. Black Arrow Online ships a list of the game's shaders: a few
seconds after starting, it builds the ones not ready yet for your graphics
card in the background. A small notice in the top left shows the progress,
then "Shaders ready". This takes a few seconds and only happens again after
a graphics driver update.
Since build 24, lights (lamps, muzzle flashes, explosions) no longer need a
shader of their own, so far fewer effects hitch in a match, and the same
shaders serve every resolution: switching Resolution, even in a match,
needs no new shaders. The first start after updating to build 24 builds
the shaders once more.
To turn it off: Graphics > Precache > Precache shaders automatically.


LAG
---
If the game stutters or lags: Troubleshoot > Record Diagnostics Log, play as
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
responding" in the top left. Open it with Troubleshoot > Open Last
Freeze Report and send it to whoever shared the package. Only the newest 5
reports are kept. To turn it off: Troubleshoot > Save a report when the
game freezes.


GAME CACHE
----------
The Xbox keeps a copy of files a game reads from its disc on the console's
hard disk. When you play a changed disc image of the same game (a newer
version or a mod), Black Arrow Online notices and empties that copy by
itself, so the game reads the new disc. The first start after that takes a
little longer. Saves, accounts and game updates are not touched. If a game
still shows old files, Troubleshoot > Empty Game Cache... empties it now and
restarts the console.


RAINBOW SIX 3
-------------
Rainbow Six 3 (the game before Black Arrow) has its own updater: Play >
Old Bow - Install Game Updates. It installs the extra maps and the title
update. The title update must fit your disc, or the game will not start:
  disc version 1 (all regions)   Game Update Menu > Install Update RF
  disc version 2 (PAL German)    Game Update Menu > Install Update Ger
  disc version 3 or 4            no update, it is already on the disc
With your Rainbow Six 3 disc loaded, XB Live > Connection Check shows the
disc version and says which one to install. Skip "Old Scharins": it breaks
multiplayer with players who have Scharins v2.


NEW ACCOUNT
-----------
Xbox Live accounts for XB Online are made on xb.live.
1. Menu: XB Live > Create an Account. It opens xb.live/xb-online in your
   browser; create the account there.
2. Then go on with step 3 of FIRST TIME: sign in to the XBL Dashboard and
   link the account (Link Code gives you the code).
Your gamertag then shows at the top of the XB Live menu.

UPDATES
-------
When a newer build is out, Black Arrow Online offers it when you start.
An update replaces the launcher, the emulator and the shader list only. Your console, Xbox
Live sign-in, saves, settings and games are not touched. The previous
emulator is kept as system\xemu.previous.exe.

Help > Versions & Updates... lists every build on the releases page. Pick a
newer one to update, or an older one to go back to it if the newest gives
you trouble. Black Arrow Online asks first, closes, installs the build and
opens again. Builds known to be broken are greyed out with the reason.
Builds before 15 do not have this window: to come back from one, choose
Update now when the launcher offers the newest build at the start.
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
to fix it. Run it any time from XB Live > Connection Check.
The results are also written to system\xemu.log.


TROUBLE
-------
Setup window says "Add content pack" instead of "Start clean": this folder
  is an update download, not the full package. Get the full package from
  where Black Arrow Online is shared and start from that.
"Xbox Live not found" or sign-in fails in the game:
  - Look at the XB Online check panel (XB Live > Connection Check) and
    fix whatever it marks FIX or WARN first.
  - VPN: turn it off completely (also auto-connect and kill switch) and
    restart Black Arrow Online. A VPN answers the Xbox Live lookups itself,
    so the console never reaches XB Online. A sign: Settings > Network in
    the dashboard shows an address that is not this PC's normal one
    (for example 10.14.0.2).
  - "PC clock": Xbox Live sign-in fails when the Windows clock or time
    zone is wrong. Settings > Time: set time and time zone automatically.
  - "Game update": Play > Black Arrow - Install Game Updates.
  - "XB Online server": a firewall, antivirus, VPN or router DNS setting is
    in the way. Allow xemu through the firewall, or try another network.
  - Close any other xemu or emulator and any game that is still running,
    then start Black Arrow Online again. Another program on UDP port 3074
    switched the console's network off in older builds.
  - Sign in with the account the XBL Dashboard added for XB Online. An
    account that is not linked to xb.live does not work until you link it
    (FIRST TIME, step 3).
  - Still failing: send system\xemu.log to whoever shared the package.
Game looks wrong: Graphics, turn off "Improve render handling".
  Effects missing or flickering: turn off "Build shaders in the background".
Slow: Graphics > Resolution, choose 1x.
Stutter on a 144 Hz (or other high refresh) monitor: the game runs at 30
  frames, which divides evenly into 60, 120 or 240 Hz but not 144 or 165.
  Set the monitor to 120 Hz (or 60 Hz) in Windows display settings.
Hosting a game:
  Players on your own network (another Xbox or PC) can join as it is. Allow
  xemu through Windows Firewall when Windows asks.
  Players over the internet can only join if your router forwards UDP port
  3074 to this PC.
  Only one copy of Black Arrow Online per PC can host.
