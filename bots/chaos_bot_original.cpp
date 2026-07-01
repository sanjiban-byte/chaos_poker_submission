// Monte Carlo Equity Bot for Chaos Poker
// 
//  Strategy: 
//  ACTION : 1200 MC rollouts → equity vs pot odds → fold/call/raise.
//           Pre-flop hand categories guide aggression (premiums raise
//           1.5× pot; trash folds to raises). Pot commitment aware.
//  SWAP   : 300 MC rollouts per option (keep/swap-0/swap-1).
//           Swaps only when equity gain exceeds cost-scaled threshold.
//  VOTE   : 1200 MC rollouts comparing current board vs expected
//           redraw equity. Wager 5–12.5% of stack scaled to confidence.
//
//  Build  : g++ -std=c++17 -O2 -o bots/chaos_bot bots/chaos_bot.cpp
//  Launch : ./bots/chaos_bot

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <cstring>
#include <cstdint>

static const int NO_CARD = -1;
static const int MAX_SEATS = 10;

// Card helpers
int parse_card(const std::string& s) {
    if (s.size() < 2) return NO_CARD;
    static const char RANKS[] = "23456789TJQKA";
    static const char SUITS[] = "shdc";
    const char* rp = std::strchr(RANKS, s[0]);
    const char* sp = std::strchr(SUITS, s[1]);
    if (!rp || !sp) return NO_CARD;
    return int(rp - RANKS) * 4 + int(sp - SUITS);
}

// Hand evaluator 
static inline uint32_t tb(int a,int b=0,int c=0,int d=0,int e=0){
    return((uint32_t)a<<16)|((uint32_t)b<<12)|((uint32_t)c<<8)|((uint32_t)d<<4)|(uint32_t)e;
}

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

uint32_t eval7(const int cards[7]) {
    static const int C[21][5]={
        {0,1,2,3,4},{0,1,2,3,5},{0,1,2,3,6},{0,1,2,4,5},{0,1,2,4,6},
        {0,1,2,5,6},{0,1,3,4,5},{0,1,3,4,6},{0,1,3,5,6},{0,1,4,5,6},
        {0,2,3,4,5},{0,2,3,4,6},{0,2,3,5,6},{0,2,4,5,6},{0,3,4,5,6},
        {1,2,3,4,5},{1,2,3,4,6},{1,2,3,5,6},{1,2,4,5,6},{1,3,4,5,6},
        {2,3,4,5,6}};
    uint32_t best=0;
    int h[5];
    for(int i=0;i<21;++i){
        for(int k=0;k<5;++k)h[k]=cards[C[i][k]];
        uint32_t s=eval5(h);if(s>best)best=s;
    }
    return best;
}

double mc_equity(int h0,int h1,const std::vector<int>&comm,
                 int num_opp,int n_sims,std::mt19937&rng){
    if(num_opp<=0)return 1.0;
    bool used[52]={};
    if(h0!=NO_CARD)used[h0]=true;
    if(h1!=NO_CARD)used[h1]=true;
    for(int c:comm)if(c!=NO_CARD)used[c]=true;
    int deck[52],dsz=0;
    for(int i=0;i<52;++i)if(!used[i])deck[dsz++]=i;
    int csz=(int)comm.size();
    int h_need=(h0==NO_CARD?1:0)+(h1==NO_CARD?1:0);
    int need=h_need+(5-csz)+2*num_opp;
    if(need>dsz)return 0.5;
    int wins=0,ties=0;
    int my7[7],op7[7];
    for(int sim=0;sim<n_sims;++sim){
        for(int i=0;i<need&&i<dsz-1;++i){
            int j=i+int(rng()%uint32_t(dsz-i));
            int t=deck[i];deck[i]=deck[j];deck[j]=t;
        }
        int idx=0;
        int mh0=(h0==NO_CARD)?deck[idx++]:h0;
        int mh1=(h1==NO_CARD)?deck[idx++]:h1;
        int brd[5];
        for(int i=0;i<csz;++i)brd[i]=comm[i];
        for(int i=csz;i<5;++i)brd[i]=deck[idx++];
        my7[0]=mh0;my7[1]=mh1;
        for(int i=0;i<5;++i)my7[2+i]=brd[i];
        uint32_t my_sc=eval7(my7);
        uint32_t best_opp=0;
        for(int o=0;o<num_opp;++o){
            op7[0]=deck[idx];op7[1]=deck[idx+1];idx+=2;
            for(int i=0;i<5;++i)op7[2+i]=brd[i];
            uint32_t os=eval7(op7);
            if(os>best_opp)best_opp=os;
        }
        if(my_sc>best_opp)++wins;
        else if(my_sc==best_opp)++ties;
    }
    return(wins+0.5*ties)/n_sims;
}

// Pre-flop hand strength category 
// Returns 0 (trash) to 4 (premium) based on hole cards alone
int preflop_strength(int h0, int h1) {
    if (h0 == NO_CARD || h1 == NO_CARD) return 2;
    int r0 = h0 >> 2, r1 = h1 >> 2;
    int s0 = h0 & 3,  s1 = h1 & 3;
    if (r0 < r1) { std::swap(r0, r1); std::swap(s0, s1); }
    bool suited = (s0 == s1);
    bool pair   = (r0 == r1);

    if (pair && r0 >= 10) return 4;          // JJ+
    if (pair && r0 >= 7)  return 3;          // 77-TT
    if (pair)             return 2;          // 22-66
    if (r0 == 12 && r1 >= 10) return 4;     // AJ+, AK
    if (r0 == 12 && r1 >= 8)  return 3;     // A8-AT (any)
    if (r0 == 12)              return 2;     // A2-A7
    if (r0 == 11 && r1 >= 9)  return 3;     // KQ, KJ, KT
    if (r0 == 11 && suited)   return 2;
    if (suited && r0 - r1 <= 4 && r0 >= 8) return 2; // suited connectors
    if (r0 >= 10 && r1 >= 9)  return 2;     // QJ, KQ offsuit etc
    if (r0 + r1 >= 18)        return 1;     // marginal
    return 0;
}

// State
struct State {
    int  my_seat      = -1;
    int  num_players  =  0;
    int  swap_mult[4] = {};
    std::mt19937 rng{std::random_device{}()};
    int  chips[MAX_SEATS]      = {};
    bool eliminated[MAX_SEATS] = {};
    bool folded[MAX_SEATS]     = {};
    int  hole[2]               = { NO_CARD, NO_CARD };
    std::vector<int> community;
    int  prior_csz  = 0;
    int  small_blind = 1;
    int  last_swap_idx = -1;

    void reset_hand() {
        hole[0] = hole[1] = NO_CARD;
        community.clear();
        prior_csz = 0;
        last_swap_idx = -1;
        for (int i = 0; i < num_players; ++i) folded[i] = false;
    }

    int active_opp() const {
        int n = 0;
        for (int i = 0; i < num_players; ++i)
            if (i != my_seat && !eliminated[i] && !folded[i]) ++n;
        return n;
    }
};

// Decisions
std::string decide_action(State& s, int chips, int cur_bet,
                          int my_bet, int min_raise, int pot) {
    if (chips <= 0) return "FOLD";
    int    num_opp = std::max(1, s.active_opp());
    double eq      = mc_equity(s.hole[0], s.hole[1], s.community,
                               num_opp, 1200, s.rng);
    int    to_call = cur_bet - my_bet;
    double pot_odds= to_call > 0 ? double(to_call)/(pot+to_call) : 0.0;

    // Pot commitment: if already invested > 30% of stack, lean toward calling
    double commitment = double(my_bet) / (chips + my_bet + 1);

    auto try_raise = [&](int num, int den) -> std::string {
        int target = cur_bet + pot * num / den;
        target = std::max(target, min_raise);
        target = std::min(target, chips + my_bet);
        if (target >= min_raise && target <= chips + my_bet)
            return "RAISE " + std::to_string(target);
        return {};
    };

    // Pre-flop: use category for aggression
    bool is_preflop = s.community.empty();
    int  pf_cat     = preflop_strength(s.hole[0], s.hole[1]);

    if (to_call > 0 && to_call >= chips) {
        // All-in call: call if equity > pot_odds + 0.02, or pot-committed
        double thresh = commitment > 0.35 ? pot_odds : pot_odds + 0.02;
        return (eq > thresh) ? "ALLIN" : "FOLD";
    }

    if (to_call <= 0) {
        // Check or raise
        double raise_thresh = is_preflop
            ? (pf_cat >= 3 ? 0.55 : pf_cat >= 2 ? 0.65 : 1.0)
            : 0.62;
        if (eq > raise_thresh) {
            auto r = try_raise(3, 2); if (!r.empty()) return r;  // 1.5x pot
            auto r2 = try_raise(1, 1); if (!r2.empty()) return r2;
        }
        if (eq > raise_thresh - 0.10) {
            auto r = try_raise(1, 2); if (!r.empty()) return r;  // half pot
        }
        return "CHECK";
    } else {
        // Facing a bet
        // Tighter fold threshold: only fold if clearly behind
        double call_thresh = pot_odds + 0.03;
        // Loosen up if pot-committed
        if (commitment > 0.35) call_thresh = pot_odds - 0.05;

        if (eq > 0.65 && eq > pot_odds + 0.12) {
            auto r = try_raise(3, 2); if (!r.empty()) return r;
            auto r2= try_raise(1, 1); if (!r2.empty()) return r2;
            return "CALL";
        }
        if (eq > call_thresh) return "CALL";

        // Pre-flop: never fold premiums to a reasonable raise
        if (is_preflop && pf_cat >= 3 && to_call < chips / 3) return "CALL";

        return "FOLD";
    }
}

std::string decide_swap(State& s, int cost, int chips_avail) {
    int num_opp = std::max(1, s.active_opp());
    const int N = 300;
    double base = mc_equity(s.hole[0], s.hole[1], s.community, num_opp, N, s.rng);
    double e0   = mc_equity(NO_CARD,   s.hole[1], s.community, num_opp, N, s.rng);
    double e1   = mc_equity(s.hole[0], NO_CARD,   s.community, num_opp, N, s.rng);
    int    best_idx = (e0 >= e1) ? 0 : 1;
    double gain     = std::max(e0, e1) - base;
    double threshold= std::max(0.03, 1.2 * cost / double(chips_avail + 1));
    if (gain > threshold) {
        s.last_swap_idx = best_idx;
        return "SWAP " + std::to_string(best_idx);
    }
    return "STAY";
}

std::string decide_vote(State& s, int chips_avail) {
    int num_opp = std::max(1, s.active_opp());
    // 1200 rollouts for accurate vote direction (2x more than original)
    const int N = 1200;
    double cur_eq = mc_equity(s.hole[0], s.hole[1], s.community, num_opp, N, s.rng);
    std::vector<int> prior_comm(s.community.begin(),
                                s.community.begin() + s.prior_csz);
    double rdraw_eq = mc_equity(s.hole[0], s.hole[1], prior_comm, num_opp, N, s.rng);
    double diff     = cur_eq - rdraw_eq;
    const char* side= (diff >= 0) ? "YES" : "NO";
    double conf     = std::abs(diff);
    // Scale wager with confidence: up to 12.5% of stack when very confident,
    // only 5% when uncertain. This avoids chip-bleed on borderline votes.
    int    max_w    = (conf > 0.10) ? chips_avail / 8 : chips_avail / 20;
    int    wager    = std::min(int(conf * max_w * 4), max_w);
    wager = std::max(0, wager);
    return std::string("VOTE ") + side + " " + std::to_string(wager);
}

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

        if (cmd == "GAME_START") {
            int sc;
            iss >> s.num_players >> s.my_seat >> sc
                >> s.swap_mult[0] >> s.swap_mult[1]
                >> s.swap_mult[2] >> s.swap_mult[3];
            for (int i = 0; i < s.num_players; ++i) s.chips[i] = sc;
        }
        else if (cmd == "HAND_START") {
            int h, d, sb, bb;
            iss >> h >> d >> sb >> bb >> s.small_blind;
            int bb_amt; iss >> bb_amt;
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
            s.prior_csz = (int)s.community.size();
            std::string c1,c2,c3; iss>>c1>>c2>>c3;
            s.community.push_back(parse_card(c1));
            s.community.push_back(parse_card(c2));
            s.community.push_back(parse_card(c3));
        }
        else if (cmd=="DEAL_TURN"||cmd=="DEAL_RIVER") {
            s.prior_csz = (int)s.community.size();
            std::string c1; iss>>c1;
            s.community.push_back(parse_card(c1));
        }
        else if (cmd=="REDRAW_FLOP") {
            std::string c1,c2,c3; iss>>c1>>c2>>c3;
            s.community.resize(s.prior_csz);
            s.community.push_back(parse_card(c1));
            s.community.push_back(parse_card(c2));
            s.community.push_back(parse_card(c3));
        }
        else if (cmd=="REDRAW_TURN"||cmd=="REDRAW_RIVER") {
            std::string c1; iss>>c1;
            s.community.resize(s.prior_csz);
            s.community.push_back(parse_card(c1));
        }
        else if (cmd=="SWAP_RESULT") {
            std::string c1; iss>>c1;
            if (s.last_swap_idx==0||s.last_swap_idx==1)
                s.hole[s.last_swap_idx]=parse_card(c1);
            s.last_swap_idx=-1;
        }
        else if (cmd=="ACTION") {
            int seat; std::string act; iss>>seat>>act;
            if (act=="FOLD") s.folded[seat]=true;
        }
        else if (cmd=="ELIMINATE") {
            int seat; iss>>seat; s.eliminated[seat]=true;
        }
        else if (cmd=="GAME_OVER") { break; }
        else if (cmd=="ACTION_PROMPT") {
            int chips,cur,mb,mr,pot;
            iss>>chips>>cur>>mb>>mr>>pot;
            s.chips[s.my_seat]=chips;
            std::cout<<decide_action(s,chips,cur,mb,mr,pot)<<std::endl;
        }
        else if (cmd=="SWAP_PROMPT") {
            int cost,chips; iss>>cost>>chips;
            s.chips[s.my_seat]=chips;
            std::cout<<(cost>chips?"STAY":decide_swap(s,cost,chips))<<std::endl;
        }
        else if (cmd=="VOTE_PROMPT") {
            int chips; iss>>chips;
            s.chips[s.my_seat]=chips;
            std::cout<<decide_vote(s,chips)<<std::endl;
        }
    }
    return 0;
}
