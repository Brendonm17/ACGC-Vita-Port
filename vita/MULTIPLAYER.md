# Multiplayer

Up to 4 players in one town: the host and 3 visitors. Everyone plays in the host's town at the same time.

## Requirements

- Everyone on the same release. Different builds get refused at the station.
- Each Vita needs its own town. Two copies of the same save can't visit each other.
- Finish Tom Nook's part-time job first, host too. Porter doesn't offer trips until then.
- Visitors can't sail to the island until someone from the host's town has named it.

## Connecting

Talk to Porter, say you're going on a trip, then pick **Visit a friend.** or **Have visitors.**

| Option | Uses | Notes |
|--------|------|-------|
| Right beside me! | Vita local wireless (ad hoc) | No router or Wi-Fi connection needed. The system asks to start local wireless. |
| On my Wi-Fi. | Your home network | Finds open towns on the same network by itself. |
| Far away. | Internet | Host gets a ticket number, visitors type it in. Host needs a port open, see below. |

The host picks the same option the visitors will use. A far-away host also shows up in the Wi-Fi search, so people in the same house pick **On my Wi-Fi.** and join alongside the far-away visitors. Local wireless doesn't mix with the other two.

On Wi-Fi and far away, Porter gives the host a ticket number (8 characters) once the line is open. Talk to Porter again to see who's visiting, get the ticket again or close the line.

## Ports

| Port | Protocol | Used for |
|------|----------|----------|
| 19520 | UDP | Game traffic to the host. 19521-19523 if 19520 is taken. |
| 19519 | UDP | Finding towns on the same Wi-Fi (broadcast). |

Only the host needs a port open. Visitors don't.

Hosting **Far away.** the Vita asks your router for the port itself, NAT-PMP first, then UPnP. The mapping lasts 2 hours, gets renewed while the line is open and is removed when you close it.

If the router says no, Porter tells you and gives you the port. Forward that UDP port to your Vita's local IP by hand. Give the Vita a fixed IP (DHCP reservation) or the forward breaks when the IP changes.

If Porter says your line "runs through another station", your router sits behind another router or your ISP's shared address (CGNAT). You can't host far away on that network. You can still visit far-away towns, and host with **Right beside me!** or **On my Wi-Fi.**

Wi-Fi search finds nothing? The search is a broadcast and some routers don't pass those between devices. Pick **Use a ticket.** when Porter offers it and type the host's ticket. Guest networks usually keep devices apart completely, then neither works. Use the main network or **Right beside me!**

## Settings

Title screen > Options > Online. Press Start to save (the game restarts). Also in `ux0:data/AnimalCrossing/settings.ini` under `[Online]`.

| Setting | Default | |
|---------|---------|---|
| Ask before visitors join | No | Far-away visitors are always asked about. Yes asks about everyone. |
| Chat in my town | On | Off turns chat off for everyone in your town. |
| Chat keyboard | GameCube | Vita uses the system keyboard. |
| Visitors act as residents | Yes | Museum, bank and so on. No is the original visitor rules. |
| Visitors pick up items | Yes | Pick up, drop, dig up, bury, plant. |
| Visitors dig holes | Yes | |
| Visitors cut trees | Yes | |
| Visitors change the tune | Yes | |
| Visitors post notes | Yes | Bulletin board. |
| Visitors use the cottage | Yes | Island cottage. |
| Visitors change designs | Yes | Able Sisters displays and the island flag. |

The host's settings are the ones that count in their town.

## Chat

Tap the balloon in the bottom left. 32 characters max. The game's own word filter applies. Tap the balloon again to close the keyboard without sending.

## Saves

- Multiplayer doesn't change the save format. The save is still a normal GameCube .gci.
- While you're visiting, your character is also kept in `mp_passport_p*_0.bin` / `mp_passport_p*_1.bin` next to your town's .gci. They're deleted once you're home and saved. Don't delete them during a trip.
- If your Vita crashes or runs out of battery mid-visit, the next boot brings your character home as of the last passport write.
- Anything you pick up in someone's town is only yours once the host's game has saved it. It does that by itself within a few seconds, and Porter waits for it when you leave. If the host's game closes or crashes before then, those items stay in their town.
- Villagers don't move between towns online.
- Back up your save folder before playing online.

## Leaving

- Visitors leave through Porter.
- The host closes the line through Porter (**Close the line.**). Visitors get 60 seconds, then the last train takes them home.
- The host saving and quitting sends everyone home.
- If a visitor's connection drops, their spot is held for 60 seconds. After that they're sent home.
- Sleep ends a local wireless session. On Wi-Fi or far away, wake up within 60 seconds and you carry on.

## Network use

Worked out from the game's send rates, not measured: about 15-20 KB/s from the host to each visitor and 5 KB/s back, so a host with 3 visitors uploads around 60 KB/s. A bad connection shows up as other players jumping around.

## Not shared yet

- Kapp'n's boat ride only plays on the rider's screen. Everyone keeps their own boat so nobody gets stranded.
- Inside shops Nook and Mable can stand in different spots on each screen.
- If the host is in a menu or a conversation when an event is due, it starts for visitors once the host is done.
- Gracie's car wash: how clean the car looks isn't shared between screens.

## Risks

- Hosting far away opens a UDP port on your router while the line is open. The ticket has your public IP in it. Only give it to people you trust.
- If the game crashes while hosting, the mapping stays in the router until it runs out (2 hours). On routers that only take permanent mappings it stays until you remove it.
- Visitors can change your town (items, holes, trees, tune, notes, designs) unless you turn those off.
- This is new. Back up your saves.

## Problems

| Problem | Try |
|---------|-----|
| "different timetable" | Everyone needs the same release. |
| Wi-Fi search finds nothing | Host opens the line first. Then try **Use a ticket.** (see Ports). |
| Far away visitors can't get in | Check the port forward. Behind CGNAT you can't host far away. |
| Porter says the ticket is for a station "in your very own home" | You and the host are on the same router, and it can't loop a far-away ticket back in. Pick **On my Wi-Fi.** instead. |
| Porter says your Vita is "still finding its Wi-Fi again" | Normal for up to 20 seconds after local wireless. Wait, then talk to him again. |

Errors go to `ux0:data/AnimalCrossing/error.log`, include it with bug reports. It starts over every launch, so copy it before you start the game again.
