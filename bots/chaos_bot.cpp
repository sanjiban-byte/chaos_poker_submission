// ============================================================
//  chaos_bot.cpp  —  Monte Carlo Equity Bot for Chaos Poker
//
//  Strategy summary
//  ─────────────────
//  ACTION  : Run 2000 MC rollouts to estimate equity; compare to
//            pot odds to decide fold/call/raise.
//  SWAP    : Run 500 MC rollouts each for "swap card 0", "swap card 1",
//            and "keep both"; swap whichever improves equity most
//            if the improvement outweighs the relative cost.
//  VOTE    : Compare MC equity on the current board vs. an MC equity
//            where the current street's cards are re-randomised.
//            Vote YES/NO accordingly; wager proportional to confidence.
//
//  Build   : g++ -std=c++17 -O2 -o bots/chaos_bot bots/chaos_bot.cpp
//  Run     : (handled by the chaos_poker engine — see README)
// ============================================================

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <cstring>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
//  Card encoding
//  card  = rank * 4 + suit
//  rank  :  0=2  1=3  2=4  3=5  4=6  5=7  6=8  7=9
//           8=T  9=J  10=Q 11=K 12=A
//  suit  :  0=s  1=h  2=d  3=c
//  NO_CARD = -1 (unknown / not yet dealt)
// ──────────────────────────────────────────────────────────────
static const int NO_CARD   = -1;
static const int MAX_SEATS = 10;

int parse_card(const std::string& s) {
    if (s.size() < 2) return NO_CARD;
    static const char RANKS[] = "23456789TJQKA";
    static const char SUITS[] = "shdc";
    const char* rp = std::strchr(RANKS, s[0]);
    const char* sp = std::strchr(SUITS, s[1]);
    if (!rp || !sp) return NO_CARD;
    return int(rp - RANKS) * 4 + int(sp - SUITS);
}

// ──────────────────────────────────────────────────────────────
//  Hand evaluator
//  Returns uint32_t — higher is better.
//  Bits [23:20] = category  (8=SF, 7=4K, 6=FH, 5=Fl,
//                            4=St, 3=3K, 2=2P, 1=1P, 0=HC)
//  Bits [19:0]  = tiebreaker nibbles (rank nibbles, MSB first)
// ──────────────────────────────────────────────────────────────

// Pack up to 5 rank nibbles into a tiebreaker word (unused = 0)
static inline uint32_t tb(int a, int b = 0, int c = 0, int d = 0, int e = 0) {
    return ((uint32_t)a << 16) | ((uint32_t)b << 12) |
           ((uint32_t)c <<  8) | ((uint32_t)d <<  4) | (uint32_t)e;
}

uint32_t eval5(const int c[5]) {
    int r[5], su[5];
    for (int i = 0; i < 5; ++i) { r[i] = c[i] >> 2; su[i] = c[i] & 3; }

    // Flush: all same suit (check before sorting)
    bool flush = su[0]==su[1] && su[1]==su[2] && su[2]==su[3] && su[3]==su[4];

    // Sort ranks descending for straight detection and tiebreaking
    std::sort(r, r + 5, std::greater<int>());

    bool all_diff = r[0]!=r[1] && r[1]!=r[2] && r[2]!=r[3] && r[3]!=r[4];
    bool str8     = all_diff && (r[0] - r[4] == 4);
    bool wheel    = r[0]==12 && r[1]==3 && r[2]==2 && r[3]==1 && r[4]==0;
    bool straight = str8 || wheel;
    int  sh       = str8 ? r[0] : 3;   // straight high (wheel = 5-high = rank 3)

    // Count rank frequencies, then extract and sort groups
    int cnt[13] = {};
    for (int i = 0; i < 5; ++i) cnt[r[i]]++;

    int gc[5] = {}, gr[5] = {}, ng = 0;
    for (int rk = 12; rk >= 0; --rk)
        if (cnt[rk]) { gc[ng] = cnt[rk]; gr[ng] = rk; ++ng; }

    // Sort groups: count desc, then rank desc (simple insertion sort on ≤5 items)
    for (int i = 0; i < ng - 1; ++i)
        for (int j = i + 1; j < ng; ++j)
            if (gc[j] > gc[i] || (gc[j] == gc[i] && gr[j] > gr[i])) {
                std::swap(gc[i], gc[j]);
                std::swap(gr[i], gr[j]);
            }

    // Determine category and pack score
    if (straight && flush) return (8u << 20) | tb(sh);
    if (gc[0] == 4)        return (7u << 20) | tb(gr[0], gr[1]);
    if (gc[0]==3 && gc[1]==2) return (6u << 20) | tb(gr[0], gr[1]);
    if (flush)             return (5u << 20) | tb(r[0], r[1], r[2], r[3], r[4]);
    if (straight)          return (4u << 20) | tb(sh);
    if (gc[0] == 3)        return (3u << 20) | tb(gr[0], gr[1], gr[2]);
    if (gc[0]==2 && gc[1]==2) return (2u << 20) | tb(gr[0], gr[1], gr[2]);
    if (gc[0] == 2)        return (1u << 20) | tb(gr[0], gr[1], gr[2], gr[3]);
    return                          tb(r[0], r[1], r[2], r[3], r[4]); // high card
}

// Best 5-of-7 hand
uint32_t eval7(const int cards[7]) {
    static const int C[21][5] = {
        {0,1,2,3,4},{0,1,2,3,5},{0,1,2,3,6},{0,1,2,4,5},{0,1,2,4,6},
        {0,1,2,5,6},{0,1,3,4,5},{0,1,3,4,6},{0,1,3,5,6},{0,1,4,5,6},
        {0,2,3,4,5},{0,2,3,4,6},{0,2,3,5,6},{0,2,4,5,6},{0,3,4,5,6},
        {1,2,3,4,5},{1,2,3,4,6},{1,2,3,5,6},{1,2,4,5,6},{1,3,4,5,6},
        {2,3,4,5,6}
    };
    uint32_t best = 0;
    int h[5];
    for (int i = 0; i < 21; ++i) {
        for (int k = 0; k < 5; ++k) h[k] = cards[C[i][k]];
        uint32_t s = eval5(h);
        if (s > best) best = s;
    }
    return best;
}

// ──────────────────────────────────────────────────────────────
//  Monte Carlo equity estimation
//
//  h0, h1    : my hole cards  (NO_CARD → dealt randomly per sim)
//  comm      : currently-known community cards  (0 to 5 elements)
//  num_opp   : active opponents in this hand
//  n_sims    : number of rollouts
//
//  Returns fraction of pots won; ties count as 0.5.
//
//  Speed note: at -O2 each rollout costs ~1500–3000 cycles
//  (21 × 5-card evals per player).  With 5 opponents and 2000
//  sims the whole call finishes in ≈ 4–6 ms on modern hardware.
// ──────────────────────────────────────────────────────────────
double mc_equity(int h0, int h1,
                 const std::vector<int>& comm,
                 int num_opp, int n_sims,
                 std::mt19937& rng)
{
    if (num_opp <= 0) return 1.0;

    // Build available deck
    bool used[52] = {};
    if (h0 != NO_CARD) used[h0] = true;
    if (h1 != NO_CARD) used[h1] = true;
    for (int c : comm) if (c != NO_CARD) used[c] = true;

    int deck[52], dsz = 0;
    for (int i = 0; i < 52; ++i) if (!used[i]) deck[dsz++] = i;

    int csz    = (int)comm.size();
    int h_need = (h0 == NO_CARD ? 1 : 0) + (h1 == NO_CARD ? 1 : 0);
    int need   = h_need + (5 - csz) + 2 * num_opp;
    if (need > dsz) return 0.5;  // not enough cards: return neutral

    int wins = 0, ties = 0;
    int my7[7], op7[7];

    for (int sim = 0; sim < n_sims; ++sim) {
        // Partial Fisher-Yates: only shuffle the positions we actually use
        for (int i = 0; i < need && i < dsz - 1; ++i) {
            int j = i + int(rng() % uint32_t(dsz - i));
            int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
        }
        int idx = 0;

        // Deal hole cards (randomly if unknown)
        int mh0 = (h0 == NO_CARD) ? deck[idx++] : h0;
        int mh1 = (h1 == NO_CARD) ? deck[idx++] : h1;

        // Complete the board
        int brd[5];
        for (int i = 0;   i < csz; ++i) brd[i] = comm[i];
        for (int i = csz; i < 5;   ++i) brd[i] = deck[idx++];

        // Evaluate my hand
        my7[0] = mh0; my7[1] = mh1;
        for (int i = 0; i < 5; ++i) my7[2 + i] = brd[i];
        uint32_t my_sc = eval7(my7);

        // Evaluate best opponent hand
        uint32_t best_opp = 0;
        for (int o = 0; o < num_opp; ++o) {
            op7[0] = deck[idx]; op7[1] = deck[idx + 1]; idx += 2;
            for (int i = 0; i < 5; ++i) op7[2 + i] = brd[i];
            uint32_t os = eval7(op7);
            if (os > best_opp) best_opp = os;
        }

        if      (my_sc > best_opp) ++wins;
        else if (my_sc == best_opp) ++ties;
    }
    return (wins + 0.5 * ties) / n_sims;
}

// ──────────────────────────────────────────────────────────────
//  Game state
// ──────────────────────────────────────────────────────────────
struct State {
    // Set once on GAME_START
    int  my_seat      = -1;
    int  num_players  =  0;
    int  swap_mult[4] = {};  // preflop / flop / turn / river cost multipliers
    std::mt19937 rng{std::random_device{}()};

    // Updated per message
    int  chips[MAX_SEATS]     = {};
    bool eliminated[MAX_SEATS]= {};
    bool folded[MAX_SEATS]    = {};
    int  hole[2]              = { NO_CARD, NO_CARD };
    std::vector<int> community;   // current known community cards
    int  prior_csz  = 0;          // community size before this street was dealt
    int  small_blind = 1;
    int  big_blind   = 2;
    int  last_pot    = 0;         // most recent pot seen (ACTION_PROMPT)
    int  last_swap_idx = -1;      // index of the hole card we just swapped

    void reset_hand() {
        hole[0] = hole[1] = NO_CARD;
        community.clear();
        prior_csz     = 0;
        last_swap_idx = -1;
        last_pot      = small_blind + big_blind;
        for (int i = 0; i < num_players; ++i) folded[i] = false;
    }

    // Count non-eliminated, non-folded opponents
    int active_opp() const {
        int n = 0;
        for (int i = 0; i < num_players; ++i)
            if (i != my_seat && !eliminated[i] && !folded[i]) ++n;
        return n;
    }
};

// ──────────────────────────────────────────────────────────────
//  Decision: ACTION_PROMPT
//  Equity vs pot odds → fold / call / raise
// ──────────────────────────────────────────────────────────────
std::string decide_action(State& s,
                           int chips, int cur_bet, int my_bet,
                           int min_raise, int pot)
{
    if (chips <= 0) return "FOLD";

    int    num_opp  = std::max(1, s.active_opp());
    double eq       = mc_equity(s.hole[0], s.hole[1], s.community,
                                num_opp, 2000, s.rng);
    int    to_call  = cur_bet - my_bet;
    double pot_odds = to_call > 0 ? double(to_call) / (pot + to_call) : 0.0;

    // Build a raise string targeting cur_bet + fraction*pot, clamped to legal range
    auto try_raise = [&](int num, int den) -> std::string {
        int target = cur_bet + pot * num / den;
        target = std::max(target, min_raise);
        target = std::min(target, chips + my_bet);
        if (target >= min_raise && target <= chips + my_bet)
            return "RAISE " + std::to_string(target);
        return {};
    };

    // All-in call situation (calling would use all chips)
    if (to_call > 0 && to_call >= chips)
        return (eq > pot_odds + 0.03) ? "ALLIN" : "FOLD";

    if (to_call <= 0) {
        // No bet to call — check or raise
        if (eq > 0.70) { auto r = try_raise(3, 4); if (!r.empty()) return r; }
        if (eq > 0.60) { auto r = try_raise(1, 2); if (!r.empty()) return r; }
        return "CHECK";
    } else {
        // Facing a bet — raise, call, or fold
        if (eq > 0.70 && eq > pot_odds + 0.15) {
            auto r = try_raise(1, 1);  // pot-sized raise
            if (!r.empty()) return r;
            auto r2 = try_raise(1, 2); // half-pot raise fallback
            if (!r2.empty()) return r2;
            return "CALL";
        }
        if (eq > pot_odds + 0.04) return "CALL";
        return "FOLD";
    }
}

// ──────────────────────────────────────────────────────────────
//  Decision: SWAP_PROMPT
//  Compare expected equity of swapping each card vs. keeping both.
//  Swap the card that gives the biggest improvement if gain > threshold.
// ──────────────────────────────────────────────────────────────
std::string decide_swap(State& s, int cost, int chips_avail) {
    int num_opp = std::max(1, s.active_opp());
    const int N = 500;

    double base = mc_equity(s.hole[0], s.hole[1], s.community, num_opp, N, s.rng);
    double e0   = mc_equity(NO_CARD,   s.hole[1], s.community, num_opp, N, s.rng);
    double e1   = mc_equity(s.hole[0], NO_CARD,   s.community, num_opp, N, s.rng);

    int    best_idx = (e0 >= e1) ? 0 : 1;
    double gain     = std::max(e0, e1) - base;

    // Minimum equity improvement needed scales with relative chip cost.
    // Small cost → low bar (0.04).  Large cost relative to stack → high bar.
    double threshold = std::max(0.04, 1.5 * cost / double(chips_avail + 1));

    if (gain > threshold) {
        s.last_swap_idx = best_idx;
        return "SWAP " + std::to_string(best_idx);
    }
    return "STAY";
}

// ──────────────────────────────────────────────────────────────
//  Decision: VOTE_PROMPT
//  Compare equity with the current board vs. a randomly redrawn
//  version of the current street.  Vote YES if current is better;
//  wager chips proportional to confidence.
// ──────────────────────────────────────────────────────────────
std::string decide_vote(State& s, int chips_avail) {
    int num_opp = std::max(1, s.active_opp());
    const int N = 1000;

    // Equity with ALL current community cards fixed
    double cur_eq = mc_equity(s.hole[0], s.hole[1], s.community,
                               num_opp, N, s.rng);

    // Equity with only PRE-STREET community cards fixed; current street is randomised.
    // e.g. after DEAL_TURN, prior_csz=3 → the 4th card is left random in MC.
    std::vector<int> prior_comm(s.community.begin(),
                                s.community.begin() + s.prior_csz);
    double rdraw_eq = mc_equity(s.hole[0], s.hole[1], prior_comm,
                                num_opp, N, s.rng);

    double diff  = cur_eq - rdraw_eq;
    const char* side = (diff >= 0) ? "YES" : "NO";

    // Wager: confidence × 25 % of stack, rounded down
    double conf  = std::abs(diff);
    int    max_w = chips_avail / 4;
    int    wager = std::min(int(conf * max_w * 4), max_w);
    wager = std::max(0, wager);

    return std::string("VOTE ") + side + " " + std::to_string(wager);
}

// ──────────────────────────────────────────────────────────────
//  Main loop: read engine messages, update state, respond to prompts
// ──────────────────────────────────────────────────────────────
int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    State s;
    std::string line;

    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        // ── State-update messages (no response required) ──────────────────

        if (cmd == "GAME_START") {
            int start_chips;
            iss >> s.num_players >> s.my_seat >> start_chips
                >> s.swap_mult[0] >> s.swap_mult[1]
                >> s.swap_mult[2] >> s.swap_mult[3];
            for (int i = 0; i < s.num_players; ++i) s.chips[i] = start_chips;
        }
        else if (cmd == "HAND_START") {
            int hnum, dealer, sbs, bbs;
            // HAND_START <hand_num> <dealer> <sb_seat> <bb_seat> <sb_amt> <bb_amt>
            iss >> hnum >> dealer >> sbs >> bbs >> s.small_blind >> s.big_blind;
            s.reset_hand();
        }
        else if (cmd == "CHIPS") {
            for (int i = 0; i < s.num_players; ++i) {
                iss >> s.chips[i];
                if (s.chips[i] == 0) s.eliminated[i] = true;
            }
        }
        else if (cmd == "DEAL_HOLE") {
            std::string c1, c2; iss >> c1 >> c2;
            s.hole[0] = parse_card(c1);
            s.hole[1] = parse_card(c2);
        }
        else if (cmd == "DEAL_FLOP") {
            s.prior_csz = (int)s.community.size();  // 0 before flop
            std::string c1, c2, c3; iss >> c1 >> c2 >> c3;
            s.community.push_back(parse_card(c1));
            s.community.push_back(parse_card(c2));
            s.community.push_back(parse_card(c3));
        }
        else if (cmd == "DEAL_TURN" || cmd == "DEAL_RIVER") {
            s.prior_csz = (int)s.community.size();  // 3 before turn, 4 before river
            std::string c1; iss >> c1;
            s.community.push_back(parse_card(c1));
        }
        // Redraw: remove current street's cards, replace with the new ones
        else if (cmd == "REDRAW_FLOP") {
            std::string c1, c2, c3; iss >> c1 >> c2 >> c3;
            s.community.resize(s.prior_csz);
            s.community.push_back(parse_card(c1));
            s.community.push_back(parse_card(c2));
            s.community.push_back(parse_card(c3));
        }
        else if (cmd == "REDRAW_TURN" || cmd == "REDRAW_RIVER") {
            std::string c1; iss >> c1;
            s.community.resize(s.prior_csz);
            s.community.push_back(parse_card(c1));
        }
        else if (cmd == "SWAP_RESULT") {
            // Update the hole card we just swapped
            std::string c1; iss >> c1;
            if (s.last_swap_idx == 0 || s.last_swap_idx == 1)
                s.hole[s.last_swap_idx] = parse_card(c1);
            s.last_swap_idx = -1;
        }
        else if (cmd == "ACTION") {
            int seat; std::string act; iss >> seat >> act;
            if (act == "FOLD") s.folded[seat] = true;
        }
        else if (cmd == "ELIMINATE") {
            int seat; iss >> seat;
            s.eliminated[seat] = true;
        }
        else if (cmd == "GAME_OVER") {
            break;
        }
        // VOTE_RESULT, WINNER, SHOWDOWN, SWAP_DONE → informational, no action

        // ── Prompts: must respond before 10 ms elapses ────────────────────

        else if (cmd == "ACTION_PROMPT") {
            int chips, cur_bet, my_bet, min_raise, pot;
            iss >> chips >> cur_bet >> my_bet >> min_raise >> pot;
            s.chips[s.my_seat] = chips;
            s.last_pot         = pot;
            std::cout << decide_action(s, chips, cur_bet, my_bet, min_raise, pot)
                      << std::endl;
        }
        else if (cmd == "SWAP_PROMPT") {
            int cost, chips; iss >> cost >> chips;
            s.chips[s.my_seat] = chips;
            // Can't swap if we can't afford it
            std::cout << (cost > chips ? "STAY" : decide_swap(s, cost, chips))
                      << std::endl;
        }
        else if (cmd == "VOTE_PROMPT") {
            int chips; iss >> chips;
            s.chips[s.my_seat] = chips;
            std::cout << decide_vote(s, chips) << std::endl;
        }
    }
    return 0;
}
