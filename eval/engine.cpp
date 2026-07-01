// Scoped Chaos Poker engine for offline evaluation.
//
// SCOPE (documented honestly in README):
//   FAITHFUL : multi-player no-limit betting, all-in + side pots, blind
//              escalation (RULES 2.3), 7-card showdown w/ standard rankings,
//              multi-hand match flow, elimination + revolution limit.
//   SIMPLIFIED: swap phase = single round (one SWAP_PROMPT per active player,
//              one optional swap each, no multi-round cascade); vote phase =
//              single round, money-weighted, ties->YES, redraw on NO majority.
//              These still exercise the bot's SWAP/VOTE handlers.
//
// This is a reimplementation of the PUBLIC spec (RULES.md) for evaluation,
// NOT Jump's harness. Win-rates are relative to this engine + the opponent
// pool, not absolute.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <random>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include <chrono>

// ----------------------------- card + eval -----------------------------
// Card int = rank*4 + suit, rank 0..12 (2..A), suit 0..3 (s,h,d,c).

static const char RANKS[] = "23456789TJQKA";
static const char SUITS[] = "shdc";

std::string card_str(int c) {
    std::string s; s += RANKS[c >> 2]; s += SUITS[c & 3]; return s;
}

static inline uint32_t tb(int a,int b=0,int c=0,int d=0,int e=0){
    return((uint32_t)a<<16)|((uint32_t)b<<12)|((uint32_t)c<<8)|((uint32_t)d<<4)|(uint32_t)e;
}

// Same evaluator as the bot (kept identical so engine + bot agree on rankings).
uint32_t eval5(const int c[5]) {
    int r[5], su[5];
    for (int i=0;i<5;++i){r[i]=c[i]>>2;su[i]=c[i]&3;}
    bool flush=su[0]==su[1]&&su[1]==su[2]&&su[2]==su[3]&&su[3]==su[4];
    std::sort(r,r+5,std::greater<int>());
    bool all_diff=r[0]!=r[1]&&r[1]!=r[2]&&r[2]!=r[3]&&r[3]!=r[4];
    bool str8=all_diff&&(r[0]-r[4]==4);
    bool wheel=r[0]==12&&r[1]==3&&r[2]==2&&r[3]==1&&r[4]==0;
    bool straight=str8||wheel;
    int  sh=str8?r[0]:3;
    int cnt[13]={};
    for(int i=0;i<5;++i)cnt[r[i]]++;
    int gc[5]={},gr[5]={},ng=0;
    for(int rk=12;rk>=0;--rk)if(cnt[rk]){gc[ng]=cnt[rk];gr[ng]=rk;++ng;}
    for(int i=0;i<ng-1;++i)
        for(int j=i+1;j<ng;++j)
            if(gc[j]>gc[i]||(gc[j]==gc[i]&&gr[j]>gr[i])){
                std::swap(gc[i],gc[j]);std::swap(gr[i],gr[j]);}
    if(straight&&flush) return(8u<<20)|tb(sh);
    if(gc[0]==4)        return(7u<<20)|tb(gr[0],gr[1]);
    if(gc[0]==3&&gc[1]==2)return(6u<<20)|tb(gr[0],gr[1]);
    if(flush)           return(5u<<20)|tb(r[0],r[1],r[2],r[3],r[4]);
    if(straight)        return(4u<<20)|tb(sh);
    if(gc[0]==3)        return(3u<<20)|tb(gr[0],gr[1],gr[2]);
    if(gc[0]==2&&gc[1]==2)return(2u<<20)|tb(gr[0],gr[1],gr[2]);
    if(gc[0]==2)        return(1u<<20)|tb(gr[0],gr[1],gr[2],gr[3]);
    return tb(r[0],r[1],r[2],r[3],r[4]);
}

const char* rank_name(uint32_t score) {
    switch (score >> 20) {
        case 8: return "STRAIGHT_FLUSH"; case 7: return "FOUR_OF_A_KIND";
        case 6: return "FULL_HOUSE";     case 5: return "FLUSH";
        case 4: return "STRAIGHT";       case 3: return "THREE_OF_A_KIND";
        case 2: return "TWO_PAIR";       case 1: return "ONE_PAIR";
        default: return "HIGH_CARD";
    }
}

uint32_t eval7(const std::array<int,7>& cards) {
    static const int C[21][5]={
        {0,1,2,3,4},{0,1,2,3,5},{0,1,2,3,6},{0,1,2,4,5},{0,1,2,4,6},
        {0,1,2,5,6},{0,1,3,4,5},{0,1,3,4,6},{0,1,3,5,6},{0,1,4,5,6},
        {0,2,3,4,5},{0,2,3,4,6},{0,2,3,5,6},{0,2,4,5,6},{0,3,4,5,6},
        {1,2,3,4,5},{1,2,3,4,6},{1,2,3,5,6},{1,2,4,5,6},{1,3,4,5,6},
        {2,3,4,5,6}};
    uint32_t best=0; int h[5];
    for(int i=0;i<21;++i){
        for(int k=0;k<5;++k)h[k]=cards[C[i][k]];
        uint32_t sc=eval5(h); if(sc>best)best=sc;
    }
    return best;
}

// ----------------------------- bot process -----------------------------
struct Bot {
    pid_t pid = -1;
    int   to_bot = -1;    // engine writes here (bot's stdin)
    int   from_bot = -1;  // engine reads here (bot's stdout)
    std::string name;
    std::string leftover; // buffered bytes not yet newline-terminated

    void spawn(const std::string& path) {
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) || pipe(out_pipe)) { perror("pipe"); _exit(1); }
        pid = fork();
        if (pid == 0) {
            dup2(in_pipe[0], 0);
            dup2(out_pipe[1], 1);
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            execl(path.c_str(), path.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(in_pipe[0]); close(out_pipe[1]);
        to_bot = in_pipe[1]; from_bot = out_pipe[0];
        name = path;
    }

    void send(const std::string& msg) {
        if (to_bot < 0) return;
        std::string line = msg + "\n";
        ssize_t n = write(to_bot, line.c_str(), line.size());
        (void)n; // if bot died, read will catch it
    }

    // Read one line with a timeout (ms). Returns false on timeout/EOF/error.
    bool recv(std::string& out, int timeout_ms) {
        size_t nl;
        while ((nl = leftover.find('\n')) == std::string::npos) {
            struct pollfd pfd { from_bot, POLLIN, 0 };
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0) return false;            // timeout or error
            char buf[4096];
            ssize_t n = read(from_bot, buf, sizeof(buf));
            if (n <= 0) return false;             // EOF / dead
            leftover.append(buf, n);
        }
        out = leftover.substr(0, nl);
        leftover.erase(0, nl + 1);
        // strip trailing CR if present
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }

    void kill_and_reap() {
        if (pid > 0) { kill(pid, SIGKILL); int st; waitpid(pid, &st, 0); pid = -1; }
        if (to_bot >= 0) { close(to_bot); to_bot = -1; }
        if (from_bot >= 0) { close(from_bot); from_bot = -1; }
    }
};

// ----------------------------- game state -----------------------------
struct Player {
    int  chips = 0;
    bool eliminated = false;
    bool folded = false;
    bool allin = false;
    int  hole[2] = {-1, -1};
    int  committed = 0;       // chips put in THIS betting round
    int  total_committed = 0; // chips put in this whole HAND (for side pots)
    bool errored_phase = false; // errored in the current phase (for split rule)
};

struct Engine {
    std::vector<Bot> bots;
    std::vector<Player> P;
    int N = 0;
    int starting_chips = 0;
    int swap_mult[4] = {};
    int small_blind = 1;
    int timeout_ms = 10;
    // (overridable via CB_ENGINE_TIMEOUT_MS env for diagnostics; default 10 per spec)
    std::mt19937 rng;
    bool verbose = false;

    // deck for the current hand
    std::vector<int> deck;
    int deck_pos = 0;
    std::vector<int> board;

    Engine(uint32_t seed) : rng(seed) {}

    void broadcast(const std::string& m) {
        for (int i = 0; i < N; ++i)
            if (!P[i].eliminated) bots[i].send(m);
        if (verbose) std::cerr << "  > ALL: " << m << "\n";
    }
    void tell(int i, const std::string& m) {
        if (!P[i].eliminated) bots[i].send(m);
        if (verbose) std::cerr << "  > P" << i << ": " << m << "\n";
    }

    int draw() { return deck[deck_pos++]; }

    void shuffle_deck() {
        deck.resize(52);
        for (int i = 0; i < 52; ++i) deck[i] = i;
        for (int i = 51; i > 0; --i)
            std::swap(deck[i], deck[std::uniform_int_distribution<int>(0,i)(rng)]);
        deck_pos = 0;
    }

    int count_active() { // not folded, not eliminated
        int c = 0; for (auto& p : P) if (!p.folded && !p.eliminated) ++c; return c;
    }
    int count_can_act() { // active and not all-in
        int c = 0; for (auto& p : P) if (!p.folded && !p.eliminated && !p.allin) ++c; return c;
    }
    int count_alive() { // not eliminated
        int c = 0; for (auto& p : P) if (!p.eliminated) ++c; return c;
    }

    // ---- get a validated action from a bot; auto-fold on error/timeout ----
    // current_bet = highest committed this round; returns nothing, mutates P[i].
    void get_action(int i, int current_bet, int min_raise, int pot) {
        Player& p = P[i];
        int to_call = current_bet - p.committed;
        std::ostringstream prompt;
        prompt << "ACTION_PROMPT " << p.chips << " " << current_bet << " "
               << p.committed << " " << min_raise << " " << pot;
        tell(i, prompt.str());

        std::string resp;
        if (!bots[i].recv(resp, timeout_ms)) { autofold(i, "timeout"); return; }
        if (verbose) std::cerr << "  < P" << i << ": " << resp << "\n";

        std::istringstream iss(resp);
        std::string act; iss >> act;

        if (act == "FOLD") { do_fold(i); }
        else if (act == "CHECK") {
            if (to_call != 0) { autofold(i, "check facing bet"); return; }
            broadcast("ACTION " + std::to_string(i) + " CHECK");
        }
        else if (act == "CALL") {
            int pay = std::min(to_call, p.chips);
            commit(i, pay);
            if (p.chips == 0) { p.allin = true;
                broadcast("ACTION " + std::to_string(i) + " ALLIN " + std::to_string(p.committed)); }
            else broadcast("ACTION " + std::to_string(i) + " CALL " + std::to_string(p.committed));
        }
        else if (act == "ALLIN") {
            commit(i, p.chips);
            p.allin = true;
            broadcast("ACTION " + std::to_string(i) + " ALLIN " + std::to_string(p.committed));
        }
        else if (act == "RAISE") {
            int target; if (!(iss >> target)) { autofold(i, "raise no amount"); return; }
            // target is TOTAL bet this round. Must be >= min_raise and <= stack+committed.
            int max_total = p.committed + p.chips;
            if (target < min_raise || target > max_total) { autofold(i, "raise out of range"); return; }
            int pay = target - p.committed;
            commit(i, pay);
            if (p.chips == 0) p.allin = true;
            broadcast("ACTION " + std::to_string(i) + " RAISE " + std::to_string(p.committed));
        }
        else { autofold(i, "unknown action"); }
    }

    void commit(int i, int amt) {
        amt = std::min(amt, P[i].chips);
        P[i].chips -= amt;
        P[i].committed += amt;
        P[i].total_committed += amt;
    }
    void do_fold(int i) {
        P[i].folded = true;
        broadcast("ACTION " + std::to_string(i) + " FOLD");
    }
    void autofold(int i, const char* why) {
        P[i].folded = true;
        P[i].errored_phase = true;
        if (verbose) std::cerr << "  ! P" << i << " auto-folded (" << why << ")\n";
        broadcast("ACTION " + std::to_string(i) + " FOLD");
    }

    // ---- one betting round. Returns when bets are settled. ----
    // first_to_act: seat to start. preflop_current_bet: highest blind already in.
    void betting_round(int first_to_act, int preflop_current_bet, int& pot) {
        int current_bet = preflop_current_bet;
        int min_raise = current_bet + 2 * small_blind; // next legal total raise
        if (current_bet == 0) min_raise = 2 * small_blind;

        // "to_act" = players who still owe an action before the round can close.
        // Everyone who can act must act at least once; a raise reopens action
        // for everyone who isn't the raiser.
        std::vector<bool> to_act(N, false);
        for (int i = 0; i < N; ++i)
            if (!P[i].folded && !P[i].eliminated && !P[i].allin) to_act[i] = true;

        int seat = first_to_act;
        int safety = 0;
        while (count_active() > 1) {
            if (++safety > 100000) break;
            // find next seat that still needs to act
            int who = -1;
            for (int k = 0; k < N; ++k) {
                int t = (seat + k) % N;
                if (to_act[t] && !P[t].folded && !P[t].eliminated && !P[t].allin) { who = t; break; }
            }
            if (who == -1) break; // nobody left to act -> round closed

            int before = current_bet;
            get_action(who, current_bet, std::max(min_raise, current_bet), pot + round_committed());
            to_act[who] = false;

            if (P[who].committed > before) {
                // a raise (or all-in above current bet): reopen action for others
                int raise_size = P[who].committed - before;
                current_bet = P[who].committed;
                min_raise = current_bet + std::max(raise_size, 2 * small_blind);
                for (int i = 0; i < N; ++i)
                    if (i != who && !P[i].folded && !P[i].eliminated && !P[i].allin)
                        to_act[i] = true;
            }
            seat = next_seat_any(who);
        }
        pot += round_committed();
        for (auto& p : P) p.committed = 0;
    }

    int round_committed() { int s=0; for (auto&p:P) s+=p.committed; return s; }
    int next_seat_any(int s) {
        for (int k = 1; k <= N; ++k) {
            int t = (s + k) % N;
            if (!P[t].eliminated && !P[t].folded && !P[t].allin) return t;
        }
        return s;
    }

    int main_loop(); // defined below
};

#include "engine_game.inc"

int main(int argc, char** argv) {
    // usage: engine <seed> <hands_cap_ignored> <starting_chips> m_pf m_fl m_tn m_rv bot0 bot1 ...
    if (argc < 9) {
        std::cerr << "usage: engine <seed> <verbose0|1> <starting_chips> "
                     "<m_pf> <m_fl> <m_tn> <m_rv> <bot0> <bot1> [bot2...]\n";
        return 2;
    }
    uint32_t seed = (uint32_t)strtoul(argv[1], nullptr, 10);
    int verbose = atoi(argv[2]);
    Engine E(seed);
    E.verbose = verbose;
    if (const char* t = getenv("CB_ENGINE_TIMEOUT_MS")) E.timeout_ms = atoi(t);
    E.starting_chips = atoi(argv[3]);
    for (int k = 0; k < 4; ++k) E.swap_mult[k] = atoi(argv[4 + k]);

    std::vector<std::string> botpaths;
    for (int i = 8; i < argc; ++i) botpaths.push_back(argv[i]);
    E.N = (int)botpaths.size();
    E.bots.resize(E.N);
    E.P.resize(E.N);
    for (int i = 0; i < E.N; ++i) {
        E.bots[i].spawn(botpaths[i]);
        E.P[i].chips = E.starting_chips;
    }
    signal(SIGPIPE, SIG_IGN);

    int winner = E.main_loop();

    for (int i = 0; i < E.N; ++i) E.bots[i].kill_and_reap();
    // Print result to stdout for the rig: "RESULT <winner_seat>" or "RESULT TIE a b"
    std::cout << "RESULT " << winner << "\n";
    return 0;
}
