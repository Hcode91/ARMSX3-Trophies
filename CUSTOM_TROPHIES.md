# Custom trophy rules (ARMSX3-Trophies)

A trophy set can carry an optional rules file. With it the emulator can

* award trophies by watching the game's memory (no game code is patched), and
* "remove" trophies, e.g. ones that need online servers that no longer exist.

Without a rules file nothing changes.

## Where the file goes

The first one found is used:

1. `dev_hdd0/home/<user>/trophy_rules/<NPWRxxxxx_00>.yml` - easy to edit while testing
2. `TROPRULES.YML` inside the game's `TROPHY.TRP` - for sharing a finished pack

The file is read when the game registers its trophies, so restart the game after editing.

## Format

```yaml
game: PPU-becb727cee674b3b8ce10f8457f61b00a60cb20f   # optional, see below
original_count: 14                                   # optional, see below
remove: [5, 9]                                       # optional
trophies:
  14:
    when: "[[0xF3F20]+0x48] >= 50"
  15:
    when:
      - "u8[0x10A2C4] & 4 != 0"
      - "f32[[0xF3F20]+0x10] > 99.5"
```

* `game` - the PPU hash from the log. Memory rules only run when it matches, because
  addresses differ between game versions. `remove` works on any version.
* `original_count` - number of trophies in the official set. Ids from there on are custom.
* `remove` - these trophies are hidden from the trophy list, are never awarded, and no
  longer block the platinum. The game itself still sees them, so nothing breaks.
* `trophies` - one entry per trophy id. The trophy is awarded once every condition is true.

## Conditions

`<value> <compare> <value>`, compare being `==  !=  >=  <=  >  <`. Several conditions can be
joined with `&&` or given as a list; all must be true.

A value is built from:

* numbers: `50`, `0x48`, `99.5`
* a memory read: `[address]` reads a big-endian 32-bit number. Use `u8[..] s8[..] u16[..]
  s16[..] u32[..] s32[..] f32[..]` for other sizes and types.
* `+ - * &` and brackets `( )`

Reads can be nested to follow pointers: `[[0xF3F20]+0x48]` reads the pointer stored at
`0xF3F20`, adds `0x48` and reads the number there. If any pointer on the way is null or not
mapped, the condition is simply false.

Rules are checked about four times a second, so they suit values that stay in memory
(counters, flags, scores), not ones that only exist for a single frame.

## Online (RPCN)

Custom trophies, trophies awarded by a rule, removed trophies, and a platinum from a set that
uses rules are never sent to RPCN.

## Trophy tracer

Whenever the game itself asks for a trophy, the emulator notes which game code made the
request (once per trophy per session), in the log and in
`dev_hdd0/home/<user>/trophy_rules/<NPWRxxxxx_00>_trace.txt`:

```
trophy 3: requested from 0x3e1f0 <- 0x41a2c <- 0x40b10 (r4=... r5=... r6=...)
```

The addresses are call sites, innermost first. Looking at that code in the decrypted EBOOT
shows which values the game checks before awarding the trophy, which is the starting point
for a new rule (e.g. the same counter with a different target). It also fires for trophies
that are already unlocked, as long as the game still asks for them.

## Memory search

In-game menu → Trophies → **Memory search (custom trophies)**. It finds the address of a
number you can see in the game:

1. Type the number as it is right now (say 12 coins) and search.
2. Go back to the game and change it (13 coins).
3. Come back and filter: "it is now this number", "went up", "went down", "changed" or
   "stayed the same". Repeat until only a few addresses are left.
4. Tap a result to copy a rule line such as `when: "[0x1A2B3C] >= 13"` for the rules file.

Number types: whole number (`u32`, the usual one), small (`u16`), tiny (`u8`) and decimal
(`f32`, for things like health bars and timers).

An address found this way is a plain, fixed address. Many games keep their data at the same
place every run, but some move it; if a rule stops working after restarting the game, that
value needs a pointer rule (`[[base]+offset]`) instead, which the search cannot find yet.
