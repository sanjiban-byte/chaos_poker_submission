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

The bot uses **Monte Carlo equity estimation** as its decision core, extended with pre-flop hand categorisation and confidence-scaled vote wagering. No offline training or pre-computed tables are required; all computation happens at decision time within the 10 ms limit.

### Betting (`ACTION_PROMPT`)

1200 Monte Carlo rollouts are run per decision. Each rollout randomly assigns hole cards to active opponents and completes the board, then evaluates all hands using an exhaustive best-5-of-7 evaluator (all 21 combinations). The resulting win fraction is the equity estimate.

Pre-flop, hole cards are classified into five tiers (premium: JJ+, AK, AQ; strong: 77–TT, A8–AT, KQ–KT; playable: small pairs, suited connectors; marginal; trash). This controls the raise threshold:
- Premium hands raise at 50%+ equity threshold, 1.5× pot sizing, and never fold to small raises
- Trash hands need 100% equity to raise pre-flop (never raise), and fold to any raise
- Post-flop: raise threshold is 62% equity, sizing 1.5× pot

Pot commitment is tracked: if a player has already invested 35%+ of their stack in the current betting round, the fold threshold is loosened (pot odds − 5%) to avoid giving up committed equity.

### Swap (`SWAP_PROMPT`)

300 rollouts are run for three scenarios: keeping both cards, replacing card 0, replacing card 1. The bot swaps whichever card produces the largest equity gain, but only if that gain clears a cost-scaled threshold — `max(3%, 1.2 × cost / stack)`. This avoids burning chips on marginal swaps and protects short stacks from swap-bleeding.

### Vote (`VOTE_PROMPT`)

1200 rollouts are run twice: once with the full current board fixed, and once with only the pre-street community cards fixed (simulating a random redraw of the current street). The bot votes YES to keep if the current board is better, NO to redraw otherwise.

600 rollouts was tried earlier but 1200 performs better as vote direction noise is reduced. 

Wager sizing scales with confidence: when the equity differential exceeds 10%, up to 12.5% of the stack is wagered; below that the wager is capped at 5%. This concentrates chip spending on high-conviction votes and avoids chip-bleed on borderline decisions.

### Tradeoffs

- **Speed vs. accuracy**: rollout counts are tuned to stay well within the 10 ms wall-clock limit. Action decisions (1200 rollouts × 5 opponents = most expensive path) complete in ~3–5 ms at -O2 on modern hardware, hence safe to run it in simpler systems. 
- **Pre-flop categories vs. pure MC**: the pre-flop tiers ensure premium hands build large pots and trash hands fold cost-free, which is the dominant win-rate driver in multi-player games where pre-flop all-ins frequently decide outcomes.
- **Vote wager sizing**: wagers scale with confidence rather than being fixed, which avoids the chip-bleed of paying aggressively to redraw boards where the equity difference is marginal.
