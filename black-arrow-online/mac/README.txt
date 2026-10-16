BLACK ARROW ONLINE FOR MAC - BETA
=================================

Rainbow Six 3: Black Arrow on XB Online, on a Mac.
This is a beta and is still being tested on Macs: expect rough edges, and
please report what you find (see REPORTING A PROBLEM).


FIRST INSTALL
-------------

You need:
- The full Mac package (BlackArrowOnline-mac-beta-*-full.zip), from where
  Black Arrow Online is shared. It has the app and the Xbox files.
- Your Rainbow Six 3: Black Arrow disc image (.iso).
- A controller, if you play with one: pair it first in
  System Settings > Bluetooth.

1. Download the full package. Google Drive says it "can't scan this file for
   viruses" because the file is large: click Download anyway.

2. Unzip it (double-click it in Downloads). Safari often does this for you,
   so you may already have a BlackArrowOnline folder in Downloads.

3. Open Terminal (Applications > Utilities > Terminal).

4. Type "bash " (with the space), drag setup.sh from the BlackArrowOnline
   folder onto the Terminal window, and press Return. It looks like:

     bash /Users/you/Downloads/BlackArrowOnline/setup.sh

   If macOS asks whether Terminal may access files in your Downloads folder,
   click Allow (otherwise setup stops with "Operation not permitted").

   Doing it this way means macOS does not block the app or ask you to
   approve it in System Settings.

5. A games folder opens. Put your Black Arrow .iso in it.

6. Start Black Arrow Online from Applications. If macOS asks whether the app
   may accept incoming network connections or find devices on your local
   network, allow it: XB Online needs both.

You can delete the downloaded zip and folder afterwards.


FIRST START
-----------

The menus are at the top of the emulator window.

1. The XBL Dashboard opens. Sign in with your xb.live account.
   Need a code? Menu: XB Online > Link Code.
   No account yet? See NEW ACCOUNT below first.
2. In the dashboard: Settings > Network, set the top option to XB Online.
3. Menu: XB Online > Black Arrow - Install Game Updates, and follow the
   prompts.
4. Menu: XB Online > Play Rainbow Six 3 Black Arrow.

After that, Black Arrow Online goes straight to the game when you start it.
If the games folder is empty it opens the XBL Dashboard instead.


NEW ACCOUNT
-----------

Xbox Live accounts for XB Online are made on xb.live.
1. Menu: XB Online > Create an Account. It opens xb.live/xb-online in your
   browser; create the account there.
2. Then go on with step 1 of FIRST START: sign in to the XBL Dashboard and
   link the account (Link Code gives you the code).
Your gamertag then shows in the XB Online menu under XB LIVE ACCOUNT.


THE XB ONLINE MENU
------------------

GAMES
  Play <game>                   Start a game from the games folder.
                                No games yet: opens the games folder.
  Black Arrow - Install Game Updates
                                Black Arrow title update and map packs.
DASHBOARDS
  XBL Dashboard                 Sign in, link or fix your Xbox Live account.
  Original Xbox Dashboard       The standard Xbox dashboard (saves, settings).
XB LIVE ACCOUNT
  Account: <gamertag>           The account on this console.
  Create an Account             Opens xb.live/xb-online (see NEW ACCOUNT).
  Link Code                     Opens xb.live/link in your browser.
HELP
  Connection Check              Checks the connection to XB Online.
  Record Diagnostics Log        Records a log while you play (see REPORTING).
  Mark Lag Spike (F9)           Marks the moment in the log you are recording.
  Open Last Freeze Report       Opens the newest freeze report.
  Open Logs Folder              Opens system/logs.
  Shader Cache                  Shaders built for your Mac.

On a controller, press the Guide/Home button for the same options.
On a Mac laptop keyboard, F9 and the other F keys may need the fn key.

The RSO Settings menu next to it has the graphics settings (resolution,
shaders, performance overlay) and the diagnostics switches. Every switch
there can be turned back off if something looks wrong. On a Mac the
renderer is always OpenGL.


UPDATING
--------

New betas are on the releases page (look for "Mac Beta"):
https://github.com/Rubyxm88/Black-Arrow-Online/releases

Download BlackArrowOnline-mac-beta-*.zip, unzip it, and run its setup.sh the
same way as above. Your console, Xbox Live sign-in, saves, settings and games
are kept. (Updates will come by themselves in a later beta.)


WHERE THINGS ARE
----------------

The app:      Applications > Black Arrow Online
              (or Applications in your home folder, if your Mac account may
              not install apps; setup says which)
Your console: ~/Library/Application Support/Black Arrow Online/system
Your games:   ~/Library/Application Support/Black Arrow Online/games

To open one in Finder: Go > Go to Folder..., paste the path, press Return.

To start over with a new console (your current one, with its Xbox Live
account and saves, is lost), run in Terminal the same way as setup:

  bash setup.sh --start-over

Already have an xemu console (xbox_hdd.qcow2 and eeprom.bin) you want to
keep? Ask whoever shared the package before copying it in: it is not
handled by setup yet.


REPORTING A PROBLEM
-------------------

The easy way: double-click "Black Arrow Report" on your Desktop (setup puts
it there). If the game is stuck, do it while the game is still open. It
makes a BlackArrow-report-....zip on your Desktop with the logs, your Mac's
details and, if the game is open, what it is doing. Send that zip and a few
words on what happened to whoever shared the package.

By hand instead, send:
- Your Mac model, chip (Apple or Intel) and macOS version
  (Apple menu > About This Mac).
- What happened, and what you expected.
- system/xemu.log from your console folder above.
- For stutter: XB Online > Record Diagnostics Log, play, press F9 right after
  each lag spike, then Stop Diagnostics Log, and send the newest file in
  system/logs.
- If the game froze: XB Online > Open Last Freeze Report, and send that file.
