Robinson's Requiem - Co-op
==========================

A fan-made online co-op mode for Robinson's Requiem (Silmarils, 1994). It runs the game
natively on Windows (no DOSBox). You need your own copy of the game (GOG or any other
version): the installer copies the game files from it into this folder's "game" folder.
Nothing from the game is included in the download.

START
  Start menu > Robinson's Requiem Co-op:
    Play Solo     - the normal game
    Host Co-op    - player 1: start a new game or load one as usual
    Join Co-op    - player 2: type the host's IP address
  As soon as the host is in the game, player 2 gets the host's game state and appears
  next to player 1 with their own character: own health/body, illnesses, hunger/thirst,
  inventory and equipment, own map and computer screens. Each PC shows its own view.

ONLINE
  Over the internet the host must forward TCP port 27531 to their PC (or use a LAN/VPN
  tool such as ZeroTier, Tailscale or Radmin VPN and join with that IP). Windows may ask
  the host to allow RRQ-Coop.exe through the firewall: allow it.
  Join with  IP:port  to use another port, and host with  RRQ-Coop.exe --host <port> game.
  Option: --delay <frames> (input delay, default 3; raise it for slow connections).

CONTROLS
  Mouse as in the original (click the arrows to walk, right click to run).
  W/S = forward/back, A/D = turn left/right, Q/E = sidestep left/right
  (turn off with --nowasd). Numpad and arrow keys work as in the original.
  F1 = map, F2 = computer (save/load), F1-F10 = the action icons, Esc = cancel.
  -f on the command line = fullscreen.

SAVES
  Saves are in the "game" folder. A co-op save also writes <name>.SAV.p2 with player 2.
  Only the host can LOAD a save or start a NEW game. Uninstalling keeps your saves.

KNOWN LIMITS
  - Player 2 is drawn with the game's own human figure.
  - Cutscenes/videos are shared (both see them).

CREDITS / LICENSES
  Engine: based on ALIS by maestun and contributors (MIT, see LICENSE-alis.txt).
  SDL2 (zlib license, see LICENSE-SDL2.txt).
  Robinson's Requiem is (c) Silmarils. This mod contains none of its files.
