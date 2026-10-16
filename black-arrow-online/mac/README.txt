BLACK ARROW ONLINE FOR MAC
=========================

Rainbow Six 3: Black Arrow on XB Online, on a Mac. The Mac version is newer
than the Windows one: please report what you find (see REPORTING A PROBLEM).


FIRST INSTALL
-------------

The PDF guide that comes with the full package has the same steps with more
help. In short:

You need:
- A Mac with macOS 11 (Big Sur) or later on Intel, or macOS 14 (Sonoma) or
  later on Apple Silicon (M1, M2...). Apple menu > About This Mac shows both.
- The full Mac package (BlackArrowOnline-mac-...-full.zip), from where Black
  Arrow Online is shared. It has the app and the Xbox files.
- Your Rainbow Six 3: Black Arrow disc image (.iso).
- A controller, if you play with one: pair it first (Bluetooth settings).

1. Download the full package. Google Drive says it "can't scan this file for
   viruses" because the file is large: click Download anyway.

2. Open Downloads in Finder. If you see a BlackArrowOnline folder, go on.
   If you only see the .zip, double-click it once to unzip it. Leave the
   folder in Downloads.

3. Open Terminal: press Cmd+Space, type Terminal, press Return.

4. Copy this line exactly, paste it into Terminal (Cmd+V), press Return:

     bash "$(ls -td ~/Downloads/BlackArrowOnline*/ | head -n 1)setup.sh"

   If macOS asks whether Terminal may access files in your Downloads folder,
   click OK. Wait until it says Done. Nothing else to type.

   This installs the app in Applications, sets up the Xbox files and your
   console, and tells macOS the app is safe to open. You will not need
   "Open Anyway" in System Settings.

5. A games folder opens. Drag your Black Arrow .iso into it.

6. Open Black Arrow Online from Applications (or Launchpad). If macOS asks
   whether the app may accept incoming network connections or find devices
   on your local network, click Allow: XB Online needs both.

You can delete the downloaded zip and folder afterwards.


FIRST START
-----------

The menus are at the top of the emulator window.

1. The XBL Dashboard opens. Sign in with your xb.live account.
   Need a code? Menu: XB Live > Link Code.
   No account yet? See NEW ACCOUNT below first.
2. In the dashboard: Settings > Network, set the top option to XB Online.
3. Menu: Play > Black Arrow - Install Game Updates, and follow the
   prompts.
4. Menu: Play > Play Rainbow Six 3 Black Arrow.

After that, Black Arrow Online goes straight to the game when you start it.
If the games folder is empty it opens the XBL Dashboard instead.


NEW ACCOUNT
-----------

Xbox Live accounts for XB Online are made on xb.live.
1. Menu: XB Live > Create an Account. It opens xb.live/xb-online in your
   browser; create the account there.
2. Then go on with step 1 of FIRST START: sign in to the XBL Dashboard and
   link the account (Link Code gives you the code).
Your gamertag then shows at the top of the XB Live menu.


THE MENUS
---------

Each menu has one job. Help > Black Arrow Online Guide has all of this
inside the app, with buttons that do what it describes.

MACHINE        Pause, Screenshot, Snapshot, Settings..., Reset, Exit.
PLAY           Play <game> from the games folder (no games yet: opens the
               games folder), Load Disc, Eject Disc, Black Arrow - Install
               Game Updates, XBL Dashboard, Original Xbox Dashboard.
XB LIVE        Account: <gamertag>, Create an Account (see NEW ACCOUNT),
               Link Code, Connection Check.
BA ONLINE      Graphics (resolution, vertical sync), sound (Fast or
               Accurate) and shaders. Every switch can be turned back off if
               something looks wrong. On a Mac the renderer is always OpenGL.
VIEW           Window scaling, fullscreen and the performance overlay.
TROUBLESHOOT   Connection Check, Record Diagnostics Log, Mark Lag Spike (F9),
               Open Last Freeze Report, Shader Cache, Empty Game Cache (see
               GAME CACHE), Open Logs Folder.
HELP           Black Arrow Online Guide, Versions & Updates, About.

The build you have shows at the right of the menu bar ("BUILD 15"). An amber
dot on it means a newer build is out; click it for Versions & Updates.

On a controller, press the Guide/Home button for the same options.
On a Mac laptop keyboard, F9 and the other F keys may need the fn key.


GAME CACHE
----------

The Xbox keeps a copy of files a game reads from its disc on the console's
hard disk. When you play a changed disc image of the same game (a newer
version or a mod), Black Arrow Online empties that copy by itself, so the
game reads the new disc; the first start after that takes a little longer.
Saves, accounts and game updates are not touched. Troubleshoot > Empty Game
Cache... empties it now and restarts the console.


UPDATING
--------

Updates come by themselves. When a new build is out, a small panel in the
top left corner says so a few seconds after the app starts. Click Update
now: the app downloads it, closes, and opens again on the new build within
about a minute. Your console, Xbox Live sign-in, saves, settings and games
are kept. Help > Versions & Updates... looks right away, and can also
go back to an older build if the newest gives you trouble.

If an update cannot be installed, the app says why the next time it opens.
To update by hand instead, download BlackArrowOnline-mac-update.zip from the
newest build on the releases page into Downloads, unzip it, and paste the
same Terminal line as in step 4 above:
https://github.com/Rubyxm88/Black-Arrow-Online/releases


WHERE THINGS ARE
----------------

The app:      Applications > Black Arrow Online
              (or Applications in your home folder, if your Mac account may
              not install apps; setup says which)
Your console: ~/Library/Application Support/Black Arrow Online/system
Your games:   ~/Library/Application Support/Black Arrow Online/games

To open one in Finder: Go > Go to Folder..., paste the path, press Return.

To start over with a new console (your current one, with its Xbox Live
account and saves, is lost), paste this into Terminal instead of the line
in step 4:

  bash "$(ls -td ~/Downloads/BlackArrowOnline*/ | head -n 1)setup.sh" --start-over

Already have an xemu console (xbox_hdd.qcow2 and eeprom.bin) you want to
keep? Ask whoever shared the package before copying it in: it is not
handled by setup yet.


REPORTING A PROBLEM
-------------------

The easy way: open Applications and double-click "Black Arrow Report" (setup
puts it next to the app). If the game is stuck, do it while the game is
still open. It makes a BlackArrow-report-....zip in Downloads with the logs,
your Mac's details and, if the game is open, what it is doing. Send that
zip and a few words on what happened to whoever shared the package.

By hand instead, send:
- Your Mac model, chip (Apple or Intel) and macOS version
  (Apple menu > About This Mac).
- What happened, and what you expected.
- system/xemu.log from your console folder above.
- For stutter: Troubleshoot > Record Diagnostics Log, play, press F9 right after
  each lag spike, then Stop Diagnostics Log, and send the newest file in
  system/logs.
- If the game froze: Troubleshoot > Open Last Freeze Report, and send that file.
