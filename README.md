# Chaos Poker — Bot Submission

## Name
Sanjiban Paul 

---

## Build

Requires `g++` with C++17 support.

```bash
g++ -std=c++17 -O2 -Wall -o bots/chaos_bot bots/chaos_bot.cpp
```

Or using the provided Makefile (Linux/macOS):

```bash
make
```

---

## Run

```bash
./bots/chaos_bot
```

The bot communicates over `stdin` / `stdout` as specified in `RULES.md`.

---

## Launch command (for the test harness)

```bash
./chaos_poker --history 1000 5 15 25 50 ./bots/chaos_bot ./bots/random_bot ./bots/random_bot
```

---

## Strategy

The bot uses **Monte Carlo equity estimation** to make all three types of decisions (action, swap, vote). All computation happens at decision time, there is no offline training phase, which ensures the bot handles Chaos Poker's novel mechanics (swap cascades, vote phases, board redraws) correctly without relying on approximations built for standard hold'em.

### Betting decisions (`ACTION_PROMPT`)
2000 Monte Carlo rollouts are run per decision. Each rollout randomly assigns hole cards to opponents and completes the board, then evaluates all hands using an exhaustive best-5-of-7 evaluator. The resulting win fraction is compared against pot odds with a buffer to decide between fold, call, and raise. Raise sizing is pot-sized (≥70% equity) or half-pot (≥60% equity). 

### Swap decisions (`SWAP_PROMPT`)
500 rollouts are run for three options: keeping both cards, swapping card 0, and swapping card 1. The bot swaps whichever card gives the largest equity improvement, but only if that improvement exceeds a cost-scaled threshold (`max(4%, 1.5 × cost/stack)`). This prevents burning chips on marginal swaps and protects short stacks.

### Vote decisions (`VOTE_PROMPT`)
1000 rollouts are run twice: once with the current board fixed, and once with only the pre-street community cards fixed (simulating a redraw of the current street). The bot votes YES if the current board is better for its hand, NO otherwise. The wager is proportional to the confidence margin, capped at 25% of stack.

### Tradeoffs
- **Speed vs. accuracy**: rollout counts (2000 / 500 / 1000) are tuned to stay well within the 10ms limit on modern hardware while giving stable equity estimates. These can be increased if latency allows.
- **No opponent modelling**: the bot assumes opponents hold random cards and does not track betting patterns or prior vote behaviour. Adding opponent range narrowing based on observed actions would be a natural extension.
- **Multi-player**: opponents are treated as a field, the bot estimates the probability of beating the best opponent hand, which is the correct metric for pot-equity in multi-way pots.
