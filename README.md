# Chaos Poker Bot Submission

## Name

Sanjiban Paul

---

## Build

Requires `g++` with C++17 support (no external dependencies).

```bash
g++ -std=c++17 -O2 -o bots/chaos_bot bots/chaos_bot.cpp
```

Or using the provided Makefile on Linux/macOS:

```bash
make bots
```

---

## Launch command

```bash
./bots/chaos_bot
```

The bot reads from `stdin` and writes to `stdout` as specified in `RULES.md`.

Example match via the test harness:

```bash
./chaos_poker 1000 5 15 25 50 ./bots/chaos_bot ./bots/random_bot ./bots/random_bot
```

---

## Strategy

The bot uses **Monte Carlo equity estimation** as its decision core, extended
with pre-flop hand categorisation and confidence-scaled vote wagering. No
offline training or pre-computed tables are required; all computation happens at
decision time within the 10 ms limit.

### Betting (`ACTION_PROMPT`)

Monte Carlo rollouts are run per decision. Each rollout randomly assigns hole
cards to active opponents and completes the board, then evaluates all hands
using an exhaustive best-5-of-7 evaluator (all 21 combinations). The resulting
win fraction is the equity estimate.

The number of rollouts is bounded by a **per-decision time budget** rather than
a fixed count. Rollouts run in batches until the budget is spent, and the
decision is made on the equity estimate gathered by then. This keeps every
decision inside the 10 ms limit regardless of how many opponents are in the
hand (each additional opponent makes a rollout more expensive, since another
hand has to be dealt and evaluated).

Equity is compared against the pot odds being offered:

- **Call** when equity exceeds the price being laid.
- **Raise** (1.5x pot, or half pot as a fallback) when equity is well ahead of
  that price.
- **Fold** otherwise.

Pre-flop, a hand-strength category (premium pairs and high broadway hands down
to trash) shifts the aggression thresholds, and pot commitment is tracked so the
bot does not fold a hand it is already priced into.

### Swaps (`SWAP_PROMPT`)

For each of the two hole cards, the bot re-estimates equity as if that card were
replaced (an average unknown card), and picks the better of the two swaps. It
swaps only when the expected equity gain exceeds a threshold scaled to the swap
cost as a fraction of the current stack, so paying to swap has to be worth it.
Otherwise it stays.

### Votes (`VOTE_PROMPT`)

For the community-card vote, the bot compares its equity on the **current**
board against its equity on a **re-simulated redraw** of the current street. It
votes YES (keep) on boards that favour it and NO (redraw) on boards that don't,
and sizes the wager (roughly 5-12.5% of stack) according to how large and
confident the equity difference is.

### Tradeoffs

The bot plays its equity honestly and does not bluff, balance ranges, or model
individual opponents' tendencies - every opponent is treated as holding a
uniformly random hand. This keeps the approach robust and hard to get wrong,
at the cost of being exploitable by opponents that read and counter betting
patterns. Opponent modelling and bluffing are the natural next extensions.

---

## Test engine

`eval/` contains a self-contained simulation of the Chaos Poker engine, written
from `RULES.md`, together with a small tournament runner. It is used only for
offline evaluation of the bot and is not part of the bot itself.

It launches bots as subprocesses, speaks the stdin/stdout protocol, and plays
full matches, modelling no-limit betting, all-in side pots, blind escalation,
the swap and vote phases, and showdown. The tournament runner
(`eval/run_tournament.py`) plays many matches of the bot against a chosen
opponent pool, rotates the bot's seat each match so blind and position effects
cancel out, and reports the match win-rate with a 95% confidence interval.

```bash
python3 eval/run_tournament.py --matches 200 \
    --test-bot chaos_bot --opponents random_bot random_bot
```

This engine is a reimplementation of the public rules for local testing, not the
official harness, so results are relative to this engine and the opponent pool
rather than absolute. It was useful mainly for confirming that decisions stay
within the time limit under multi-opponent load and for comparing win-rates
across opponent counts. 