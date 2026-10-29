BLACK ARROW ONLINE FOR LINUX
============================

Rainbow Six 3: Black Arrow on XB Online, on Linux and the Steam Deck. The
Linux version is new: please report what you find (see REPORTING A PROBLEM).


FIRST INSTALL
-------------

You need:
- A 64-bit PC (x86_64) with a desktop Linux from 2022 or later: Ubuntu
  22.04, Linux Mint 21, Fedora 36, SteamOS 3, Arch... and graphics drivers
  with OpenGL 4.0 or Vulkan (any graphics card from the last ten years).
- The full Linux package (BlackArrowOnline-linux-...-full.tar.gz), from
  where Black Arrow Online is shared. It has the app and the Xbox files.
- Your Rainbow Six 3: Black Arrow disc image (.iso).

1. Download the full package. Google Drive says it "can't scan this file for
   viruses" because the file is large: click Download anyway.

2. Unpack it: right-click it in your file manager and choose Extract Here
   (Steam Deck: switch to Desktop Mode first, then use Dolphin). You get a
   BlackArrowOnline folder.

3. Open a terminal in that folder: right-click inside the BlackArrowOnline
   folder and choose Open Terminal Here (or Open in Terminal). Type this and
   press Enter:

     bash setup.sh

   Wait until it says Done. Nothing else to type, and no password: it only
   writes to your own home folder.

   This installs the app, sets up the Xbox files and your console, and adds
   Black Arrow Online and Black Arrow Report to your applications menu.

4. A games folder opens. Copy your Black Arrow .iso into it.

5. Start Black Arrow Online from your applications menu (Steam Deck: the
   Games section of the application launcher).

You can delete the downloaded file and folder afterwards.


STEAM DECK: GAME MODE
---------------------

In Desktop Mode, open the application launcher, find Black Arrow Online
under Games, right-click it and choose Add to Steam. Back in Game Mode it is
in your library under Non-Steam. The emulator's menus open with a
controller's Guide button, which the Steam button keeps for itself: in the
game's controller settings, put Guide on a back button (L4 or R4). Not yet
tried on a Steam Deck: please report how it goes.


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
               games folder), Load Disc, Eject Disc, the game update
               installers, XBL Dashboard, Original Xbox Dashboard.
XB LIVE        Account: <gamertag>, Create an Account (see NEW ACCOUNT),
               Link Code, Connection Check.
SOUND          Sound quality (Fast Enhanced or Accurate), 5.1 surround and
               Louder movement.
GRAPHICS       Resolution, Enhanced graphics, fullscreen (F11), window
               scaling, shaders, Improve render handling, performance
               overlay and renderer.
TROUBLESHOOT   Connection Check, Record Diagnostics Log, Mark Lag Spike (F9),
               Open Last Freeze Report, Shader Cache, Empty Game Cache, Open
               Logs Folder.
HELP           Black Arrow Online Guide, Versions & Updates, About.

The build you have shows at the right of the menu bar. An amber dot on it
means a newer build is out; click it for Versions & Updates.

On a controller, press the Guide/Home button for the same options.


UPDATING
--------

Updates come by themselves. When a new build is out, a small panel in the
top left corner says so a few seconds after the app starts. Click Update
now: the app downloads it, closes, and opens again on the new build. Your
console, Xbox Live sign-in, saves, settings and games are kept.
Help > Versions & Updates... looks right away, and can also go back to an
older build if the newest gives you trouble.

If an update cannot be installed, the app says why the next time it opens.
To update by hand instead, download BlackArrowOnline-linux-update.tar.gz
from the newest build on the releases page, unpack it, and run its setup.sh
as in step 3 above:
https://github.com/Rubyxm88/Black-Arrow-Online/releases


WHERE THINGS ARE
----------------

Everything is in ~/.local/share/Black Arrow Online (a hidden folder in your
home folder; Ctrl+H shows hidden folders in most file managers):

app/       the emulator (replaced by every update)
system/    your console: settings, hard disk, logs
games/     your games

To start over with a new console (your current one, with its Xbox Live
account and saves, is lost), run setup.sh like this instead:

  bash setup.sh --start-over

To remove the app and its menu entries (your console and games stay):

  bash setup.sh --uninstall


HOSTING
-------

Players over the internet can join a game you host only if your router
forwards UDP port 3074 to this computer. If you run a firewall (ufw,
firewalld), allow UDP port 3074 too.


REPORTING A PROBLEM
-------------------

The easy way: start "Black Arrow Report" from your applications menu. If the
game is stuck, do it while the game is still open. It makes a
BlackArrow-report-....tar.gz in Downloads with the logs, your computer's
details and, if the game is open, what it is doing. Send that file and a
few words on what happened to whoever shared the package.

By hand instead, send:
- Your Linux (name and version) and your graphics card.
- What happened, and what you expected.
- system/xemu.log from the folder above.
- For stutter: Troubleshoot > Record Diagnostics Log, play, press F9 right
  after each lag spike, then Stop Diagnostics Log, and send the newest file
  in system/logs.
- If the game froze: Troubleshoot > Open Last Freeze Report, and send that
  file.
