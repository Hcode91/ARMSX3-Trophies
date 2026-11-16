# Custom trophy rules (PocketTrophy)

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

## Custom trophies

New trophies are listed under `custom:`. They are added after the game's own trophies, in
order, every time the game starts; the game's `TROPHY.TRP` is never modified and the progress
file (`TROPUSR.DAT`) is resized automatically, keeping what was already earned.

```yaml
game: PPU-becb727cee674b3b8ce10f8457f61b00a60cb20f
custom:
  - name: "Pest Control"
    detail: "Kill 14 enemies in PIXEL!"
    grade: B                 # B, S or G
    icon: pest.png           # optional, in trophy_rules/<NPWRxxxxx_00>/; the game icon otherwise
    when: "[[0xF3F20]+0x24] >= 14"
```

A custom trophy's number is its position in the list, so never remove or reorder entries by
hand: that would move earned progress onto the wrong trophies. To get rid of one, add
`deleted: true` to it. It keeps its place, but is hidden and can no longer be earned.

**Platinum.** A custom trophy with `grade: P` needs no rule: it is earned when every other
trophy in the set (official and custom, not counting removed ones) is unlocked. Only for sets
that do not have a platinum already, and only one per set.

**Problems.** If something in the file cannot be used (a rule that cannot be understood, a
file made for another game version), the emulator writes what and why to
`trophy_rules/<NPWRxxxxx_00>_problems.txt` and the memory search screen shows it.

**Testing.** *Reset* locks a custom trophy again and clears its rule's progress, so a rule can
be tried over and over, ideally from a savestate made just before the moment it is about.
Changing rules and loading a savestate picks the new rules up; confirm pointer rules with a
real restart, since a savestate restores memory exactly as it was.

The memory search screen does all of this for you: **Create trophy** (tap a result), a list of
the game's custom trophies with **Edit** and **Delete**, and **Export pack** / **Import pack**,
which save or install the rules file together with its icons as one zip.

## Games without trophies

A game that never had trophy support (or a version without them, such as some original disc
releases) can still get a trophy set. Name the rules file after the game's serial instead of
a trophy ID:

```yaml
# dev_hdd0/home/<user>/trophy_rules/BLUS30256.yml
title: "50 Cent: Blood on the Sand"    # optional, the game's own title otherwise
game: PPU-...                          # optional, as for any rules file
custom:
  - name: "First blood"
    detail: "Kill an enemy"
    grade: B
    when: "[[0x123456]+0x10] >= 1"
  - name: "Completionist"
    grade: P                           # optional platinum, earned by unlocking the rest
```

When the game starts, the emulator builds the whole set from that file: a trophy folder named
after the serial, the game's own icon, and a progress file. Every trophy in it is custom, so
nothing is ever sent online. Rules, pop-ups, the trophy list, savestates, Reset, and pack
import/export all work as for any other set.

The memory search's **Create trophy** form does this for you: in a game without trophies it
writes the serial-named file, so after the first trophy is created and the game restarted,
the set exists.

If the game does register a trophy set of its own, that set is used instead and a
serial-named file is ignored.

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

### More than a plain comparison

* `delta(x)` - how much `x` has changed since the game started this session:
  `delta([[0xF3F20]+0x24]) >= 50` is "50 kills in one sitting", whatever the save had before.
* `prev(x)` - the value of `x` at the previous check, for reacting to changes:
  `[0x1000] - prev([0x1000]) >= 10` (jumped by 10 at once), `[0x1000] == 0 && prev([0x1000]) > 0`
  (just dropped to zero).
* `... for 60s` (also `for 2m`, `for 500ms`) after a comparison - it has to stay true that long:
  `f32[0x2000] > 90 for 60s`. Time spent paused does not count.
* `||` - either side is enough. `&&` binds tighter: `a && b || c` is "(a and b) or c".
* `trophy(n)` - 1 if trophy number `n` is unlocked: `trophy(14) == 1 && trophy(15) == 1` makes a
  trophy for earning a group of others.

* `button(cross)` - 1 while that controller button is held (player 1). Names: `cross` (or `x`),
  `circle`, `square`, `triangle`, `l1 l2 l3 r1 r2 r3`, `start`, `select`, `up down left right`.
* `count(x)` - how many times `x` has gone from false to true. The total is saved, so it carries
  on across sessions: `count(button(cross)) >= 1000` is "press X a thousand times".
  A comparison is allowed inside the brackets: `count([0x1000] > 5) >= 3`. To combine two things
  inside brackets use `*` for "and": `count(button(cross) * ([0x1000] == 2)) >= 100` counts
  presses only while `[0x1000]` is 2 (say, while in a level rather than a menu).

Rule files that use `button()`, `count()` or `prev()` are checked every frame rather than four
times a second, so short-lived events are not missed.

### Sequences

```yaml
  - name: "Flawless"
    steps:
      - "[0x3000] == 1"          # boss fight started
      - "[0x3004] == 0"          # then: boss health reached zero
    reset: "prev([0x3008]) > [0x3008]"   # lost health: back to the first step
```

`steps` have to come true one after another. While `reset` is true the trophy goes back to the
first step. A rule with `when` is simply a single step, and can have a `reset` too.

### Progress

A rule that is just `value >= number` shows its progress ("37 / 50") next to the trophy in the
trophy list while it is locked.

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

For a value the game never shows (a hidden "blocks destroyed" count), tap **I don't know the
number** instead of searching. The emulator remembers all of the game's memory; then do the
thing in the game and filter "went up", wait and filter "stayed the same", and keep
alternating until a list of addresses appears. This uses a few hundred MB of memory until
the list is short.

Number types: whole number (`u32`, the usual one), small (`u16`), tiny (`u8`) and decimal
(`f32`, for things like health bars and timers).

Other filters: type a number and use "up by exactly" / "down by exactly" for values that
change in known steps.

**Watching values.** Tap a result and choose "Just watch this value for now" to add it to the
watch list, which shows live values. "Show values and filter buttons over the game" puts the
watch list, the match count and Up / Down / Same / Changed buttons on top of the running game,
so a search does not need a trip to the menu each round. The watch list is saved per game:
add a few pointer candidates, restart the game, and the ones that still show the right number
are the real pointers.

An address found this way is right for the current session, but many games put their data
somewhere new each time they start. **Find pointer** on a result looks for fixed places in
the game's own data that lead to that address, directly (`[[0xF3F20]+0x48]`) or through one or
two more pointers (`[[[0xF3C18]+0x124]+0x1C]`), and lists them likeliest first. A pointer rule
keeps working after a restart; if the first one does not, try the next.
