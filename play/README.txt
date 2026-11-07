Robinson's Requiem - Co-op (test build)
=======================================

Both players need this folder and the game folder next to it (..\game = a copy of the
GOG game files). Nothing from the game is inside this folder except what you copied.

ONLINE CO-OP (2 PCs)
  Player 1 (host):  run "Host Co-op.bat". Start a new game or load one as usual.
  Player 2:         run "Join Co-op.bat" and type the host's IP address.
  As soon as the host is in the game, player 2 gets the host's game state and appears
  next to player 1 with their own character: own health/body, illnesses, hunger/thirst,
  inventory and equipment. Each PC shows its own player's view.

  Over the internet the host must forward TCP port 27531 to their PC (or use a LAN/VPN
  tool such as ZeroTier, Tailscale or Radmin VPN and join with that IP).
  Options: --delay <frames> (input delay, default 3; raise it for slow connections).

CONTROLS
  Mouse as in the original (click the arrows to walk, right click to run).
  Numpad 4-9 move/turn, Shift = run, Ctrl = crawl.  W/A/S/D also move/turn
  (turn off with --nowasd). F1-F10 = the action icons, Esc = cancel / right button.

KNOWN LIMITS (test build)
  - Saving: the in-game save (computer icon) also writes <name>.SAV.p2 with player 2;
    loading that save restores player 2 (position, body, inventory). Old saves: P2 starts fresh.
  - Player 2 is drawn as a soldier figure from the game's own graphics.
  - Cutscenes/videos are shared (both see them).
