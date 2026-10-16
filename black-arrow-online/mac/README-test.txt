BLACK ARROW ONLINE FOR MAC - TEST BUILD
=======================================

This is an early test build for Mac testers. It is not a release: there are
no automatic updates yet, and things may not work. Thanks for trying it.

Works on Apple Silicon and Intel Macs.


WHAT YOU NEED
-------------

- This test build, unzipped (you have it, since you are reading this).
- The content pack, BlackArrowOnline-content.zip, from where Black Arrow
  Online is shared. Download it in your browser like any file.
- Your Rainbow Six 3: Black Arrow disc image (.iso).


SET IT UP
---------

1. Open Terminal (Applications > Utilities > Terminal).

2. Type "bash " (with the space), drag setup-test.sh from this folder onto
   the Terminal window, type a space, drag BlackArrowOnline-content.zip onto
   the window, then press Return. It looks like:

     bash /Users/you/Downloads/BlackArrowOnline-mac-test/setup-test.sh /Users/you/Downloads/BlackArrowOnline-content.zip

   Running it this way means macOS does not ask you to approve the app in
   System Settings.

3. A games folder opens. Put your Black Arrow .iso in it.

4. Start "Black Arrow Online Test" from the Applications folder in your home
   folder (Finder > Go > Home > Applications).

The first start opens the XBL Dashboard to sign in to or set up your Xbox Live
account, the same as on Windows. If macOS asks whether the app may accept
incoming network connections or find devices on your network, allow it.


WHERE THINGS ARE
----------------

The app:      ~/Applications/Black Arrow Online Test.app
Your console: ~/Library/Application Support/Black Arrow Online/system
Your games:   ~/Library/Application Support/Black Arrow Online/games

(In Finder, Go > Go to Folder... and paste the path.)

Running setup-test.sh again with a newer test build or content pack updates
them and keeps your console, saves and settings.

To start over with a new console (your current one is lost):

  bash setup-test.sh --start-over


WHAT TO SEND BACK
-----------------

- Your Mac model and macOS version (Apple menu > About This Mac).
- Did it boot the dashboard, sign in to XB Online, and play a match?
- How smooth it felt, and when it stuttered.
- The log: system/xemu.log in your console folder above. For stutter, also
  record a diagnostics log (XB Online > Help > Record Diagnostics Log, play,
  then Stop) and send the newest file in system/logs.
