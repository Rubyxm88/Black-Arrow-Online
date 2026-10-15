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
   No account yet? See NEW ACCOUNT below first.
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
Check Connection                Runs the XB Online check again (see below).
New Account: Register Xbox      Insignia's Setup Assistant (see NEW ACCOUNT).
New Account: Get a Code         Opens insignia.live in your browser.

On a controller, press the Guide/Home button for the same options.

The XBL Dashboard is installed on the console's hard disk (clean start), so
it can update itself when it offers an update. If you brought your own hard
disk it runs from the disc image instead and stays at the bundled version.


NEW ACCOUNT
-----------
Xbox Live accounts are created on Insignia once. After that you sign in to
XB Online with it like everyone else.
1. Menu: XB Online > New Account: Get a Code. On insignia.live enter your
   email and ask for a subscription code. It arrives by email.
2. Menu: XB Online > New Account: Register Xbox. In the Setup Assistant
   press "Register Xbox" and wait for the message that it worked.
3. Menu: XB Online > Original Xbox Dashboard.
   Settings > Network Settings: the connection test should pass.
4. Back on the dashboard home, open Xbox Live > New Account:
     Real:  gamertag, billing region, the code from your email, and the
            SAME email you used on insignia.live.
     Fake:  credit card (number 4111 1111 1111 1111, any name and date)
            and phone number. Do not enter real card details.
   Set a pass code when it asks.
5. Go to step 3 of FIRST TIME: sign in to xb.live with your Insignia login
   and link the account in the XBL Dashboard.
If New Account cannot reach Xbox Live, set Machine > Settings > Network >
"Xbox Live service" to Insignia for steps 3-4, then back to XB Online.
Help: https://insignia.live/guide/connect


UPDATES
-------
When a newer build is out, Black Arrow Online offers it when you start.
An update replaces the launcher and the emulator only. Your console, Xbox
Live sign-in, saves, settings and games are not touched. The previous
emulator is kept as system\xemu.previous.exe.
If Insignia's setup disc (system\Insignia-Setup.iso) is missing, an update
also downloads it from Insignia's own GitHub releases. Without it the "New
Account" entries of the XB Online menu stay hidden.
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
A few seconds after starting, a panel in the top left corner checks what
Xbox Live needs: the console network, the XB Online server, this PC's
clock, the game disc, the game update and the Xbox Live account. When
everything is fine it disappears by itself. When something is wrong it
stays and says what to do. Run it again from XB Online > Check Connection.
The results are also written to system\xemu.log.


TROUBLE
-------
Setup window says "Add content pack" instead of "Start clean": this folder
  is an update download, not the full package. Get the full package from
  where Black Arrow Online is shared and start from that.
"Xbox Live not found" or sign-in fails in the game:
  - Look at the XB Online check panel (XB Online > Check Connection) and
    fix whatever it marks FIX or WARN first.
  - VPN: turn it off completely (also auto-connect and kill switch) and
    restart Black Arrow Online. A VPN answers the Xbox Live lookups itself,
    so the console never reaches XB Online. A sign: Settings > Network in
    the dashboard shows an address that is not this PC's normal one
    (for example 10.14.0.2).
  - "PC clock": Xbox Live sign-in fails when the Windows clock or time
    zone is wrong. Settings > Time: set time and time zone automatically.
  - "Game update": XB Online > Install Game Update + Maps.
  - "XB Online server": a firewall, antivirus, VPN or router DNS setting is
    in the way. Allow xemu through the firewall, or try another network.
  - Close any other xemu or emulator and any game that is still running,
    then start Black Arrow Online again. Another program on UDP port 3074
    switched the console's network off in older builds.
  - Sign in with the account the XBL Dashboard added for XB Online. An
    account made for Insignia only does not work on XB Online until you
    link it there (FIRST TIME, step 3).
  - Still failing: send system\xemu.log to whoever shared the package.
Game looks wrong: Machine > Settings > Display, turn off
  "Skip render target copy-back (speed hack)".
Slow: View > Int. Resolution Scale, choose 1x.
Stutter on a 144 Hz (or other high refresh) monitor: the game runs at 30
  frames, which divides evenly into 60, 120 or 240 Hz but not 144 or 165.
  Set the monitor to 120 Hz (or 60 Hz) in Windows display settings.
Hosting a game:
  Players on your own network (another Xbox or PC) can join as it is. Allow
  xemu through Windows Firewall when Windows asks.
  Players over the internet can only join if your router forwards UDP port
  3074 to this PC.
  Only one copy of Black Arrow Online per PC can host.
