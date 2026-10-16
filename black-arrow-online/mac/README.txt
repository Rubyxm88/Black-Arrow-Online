BLACK ARROW ONLINE FOR MAC - BETA
=================================

Rainbow Six 3: Black Arrow on XB Online, for Apple Silicon and Intel Macs.
This is a beta: it works for the people testing it, but expect rough edges,
and please report what you find.


FIRST INSTALL
-------------

You need:
- The full Mac package (BlackArrowOnline-mac-beta-*-full.zip), from where
  Black Arrow Online is shared. It has the app and the Xbox files.
- Your Rainbow Six 3: Black Arrow disc image (.iso).

1. Unzip the full package (double-click it). You get a BlackArrowOnline folder.

2. Open Terminal (Applications > Utilities > Terminal).

3. Type "bash " (with the space), drag setup.sh from the BlackArrowOnline
   folder onto the Terminal window, and press Return. It looks like:

     bash /Users/you/Downloads/BlackArrowOnline/setup.sh

   Doing it this way means macOS does not block the app or ask you to
   approve it in System Settings.

4. A games folder opens. Put your Black Arrow .iso in it.

5. Start Black Arrow Online from Applications.

The first start opens the XBL Dashboard to sign in to or set up your Xbox Live
account, the same as on Windows. If macOS asks whether the app may accept
incoming network connections or find devices on your network, allow it.

You can delete the downloaded zip and folder afterwards.


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
Your console: ~/Library/Application Support/Black Arrow Online/system
Your games:   ~/Library/Application Support/Black Arrow Online/games

To open one in Finder: Go > Go to Folder..., paste the path, press Return.

To start over with a new console (your current one, with its Xbox Live
account and saves, is lost):

  bash setup.sh --start-over


REPORTING A PROBLEM
-------------------

Send to whoever shared the package:
- Your Mac model and macOS version (Apple menu > About This Mac).
- What happened, and what you expected.
- system/xemu.log from your console folder above.
- For stutter: XB Online > Help > Record Diagnostics Log, play, then Stop, and
  send the newest file in system/logs.
