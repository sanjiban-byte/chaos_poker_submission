#!/usr/bin/env python3
"""
Evaluation harness for the Chaos Poker bot.

Runs many matches of (test bot) vs (opponent pool) through the simulation
engine, rotating the test bot's seat each match to cancel positional/blind
advantage, and reports the match win-rate with a 95% confidence interval.

Example:
  python3 eval/run_tournament.py --matches 200 \
      --test-bot chaos_bot_final --opponents random_bot random_bot
"""
import argparse, os, subprocess, math, sys

ENGINE = os.path.join(os.path.dirname(__file__), "engine")
BOTS   = os.path.join(os.path.dirname(os.path.dirname(__file__)), "bots")

def wilson_ci(wins, n, z=1.96):
    """Wilson score interval -- accurate at small n and near 0/1."""
    if n == 0:
        return (0.0, 0.0, 0.0)
    p = wins / n
    denom = 1 + z*z/n
    center = (p + z*z/(2*n)) / denom
    half = (z*math.sqrt(p*(1-p)/n + z*z/(4*n*n))) / denom
    return (p, center - half, center + half)

def run_match(seed, seat, test_bot, opponents, chips, mults):
    n = len(opponents) + 1
    opp_iter = iter(opponents)
    order = [test_bot if i == seat else next(opp_iter) for i in range(n)]
    cmd = [ENGINE, str(seed), "0", str(chips)] + [str(m) for m in mults] \
          + [os.path.join(BOTS, b) for b in order]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    for line in out.stdout.splitlines():
        if line.startswith("RESULT"):
            return line.split()[1] == str(seat)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--matches", type=int, default=200)
    ap.add_argument("--test-bot", default="chaos_bot_final")
    ap.add_argument("--opponents", nargs="+", default=["random_bot", "random_bot"])
    ap.add_argument("--chips", type=int, default=200)
    ap.add_argument("--mults", nargs=4, type=int, default=[5, 15, 25, 50])
    ap.add_argument("--seed-base", type=int, default=1000)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    n_players = len(args.opponents) + 1
    wins = counted = errors = 0
    for m in range(args.matches):
        seat = m % n_players
        try:
            r = run_match(args.seed_base + m, seat, args.test_bot,
                          args.opponents, args.chips, args.mults)
        except subprocess.TimeoutExpired:
            r = None
        if r is None:
            errors += 1
            continue
        counted += 1
        wins += int(r)
        if not args.quiet and (m + 1) % 50 == 0:
            p, lo, hi = wilson_ci(wins, counted)
            print(f"  [{m+1}/{args.matches}] win={p:.3f} ci=[{lo:.3f},{hi:.3f}]",
                  file=sys.stderr)

    p, lo, hi = wilson_ci(wins, counted)
    fair = 1.0 / n_players
    print(f"matches={counted} wins={wins} errors={errors}")
    print(f"win_rate={p:.4f} ci95=[{lo:.4f},{hi:.4f}] "
          f"fair_share={fair:.4f} edge={p-fair:+.4f}")
    print(f"RESULT {p:.6f} {lo:.6f} {hi:.6f} {counted}")

if __name__ == "__main__":
    main()
