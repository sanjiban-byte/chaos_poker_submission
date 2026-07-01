// engine_game.inc  -- included into engine.cpp (has access to Engine members)

// ---- swap phase (spec-compliant multi-round, per RULES section 5) ----
// Each active, non-all-in player may swap one card per round, paying cost into
// the pot. After a swap the player STAYS eligible and is prompted again; a STAY
// removes them from further rounds. Phase ends when nobody swaps in a round.
// (This is the faithful protocol -- required so spec-following bots, which
//  expect a fresh SWAP_PROMPT after each SWAP_RESULT, stay in sync.)
void swap_phase_impl(Engine& E, int street_idx, int& pot) {
    int cost = E.swap_mult[street_idx] * E.small_blind;
    std::vector<bool> eligible(E.N, false);
    for (int i = 0; i < E.N; ++i) {
        Player& p = E.P[i];
        if (!p.folded && !p.eliminated && !p.allin) eligible[i] = true;
    }
    bool any_round = true;
    int safety = 0;
    while (any_round && ++safety < 64) {
        any_round = false;
        for (int i = 0; i < E.N; ++i) {
            if (!eligible[i]) continue;
            Player& p = E.P[i];
            E.tell(i, "SWAP_PROMPT " + std::to_string(cost) + " " + std::to_string(p.chips));
            std::string resp;
            if (!E.bots[i].recv(resp, E.timeout_ms)) { eligible[i] = false; continue; }
            if (E.verbose) std::cerr << "  < P" << i << ": " << resp << "\n";
            std::istringstream iss(resp); std::string cmd; iss >> cmd;
            if (cmd == "SWAP") {
                int idx;
                if (!(iss >> idx) || (idx != 0 && idx != 1) || p.chips < cost) {
                    eligible[i] = false; continue; // invalid/unaffordable -> out
                }
                p.chips -= cost; p.total_committed += cost; pot += cost;
                int nc = E.draw();
                p.hole[idx] = nc;
                E.tell(i, "SWAP_RESULT " + card_str(nc));
                any_round = true;          // they swapped -> stay eligible
            } else {
                eligible[i] = false;       // STAY (or anything else) -> out
            }
        }
    }
    E.broadcast("SWAP_DONE");
}

// ---- simplified single-round vote phase ----
// Each active player votes YES/NO with a wager. Ties -> YES (keep). NO majority
// (by chips) -> redraw this street's community cards.
void vote_phase_impl(Engine& E, int street_idx, int prior_board_size, int& pot) {
    long yes = 0, no = 0;
    for (int i = 0; i < E.N; ++i) {
        Player& p = E.P[i];
        if (p.folded || p.eliminated) continue;
        E.tell(i, "VOTE_PROMPT " + std::to_string(p.chips));
        std::string resp;
        if (!E.bots[i].recv(resp, E.timeout_ms)) continue; // timeout: abstain
        std::istringstream iss(resp); std::string cmd, side; int amt = 0;
        iss >> cmd >> side >> amt;
        if (cmd != "VOTE") continue;
        if (amt < 0) amt = 0;
        if (amt > p.chips) amt = p.chips;
        p.chips -= amt; p.total_committed += amt; pot += amt;
        if (side == "YES") yes += amt; else if (side == "NO") no += amt;
    }
    bool redraw = no > yes;
    E.broadcast("VOTE_RESULT " + std::to_string(yes) + " " + std::to_string(no) +
                (redraw ? " REDRAWN" : " KEPT"));
    if (redraw) {
        // discard current street cards, redraw same count from remaining deck
        int cur = (int)E.board.size();
        int k = cur - prior_board_size;
        E.board.resize(prior_board_size);
        std::string msg;
        for (int t = 0; t < k; ++t) E.board.push_back(E.draw());
        if (street_idx == 1) {
            msg = "REDRAW_FLOP";
            for (int t = prior_board_size; t < (int)E.board.size(); ++t) msg += " " + card_str(E.board[t]);
        } else if (street_idx == 2) {
            msg = "REDRAW_TURN " + card_str(E.board.back());
        } else {
            msg = "REDRAW_RIVER " + card_str(E.board.back());
        }
        E.broadcast(msg);
    }
}

// ---- side-pot resolution ----
// Standard algorithm: repeatedly take the smallest positive total_committed
// among contenders as a "layer", every contributor pays min(layer, their
// contribution) into that pot, eligible winners are non-folded players who
// reached that layer. Award each layer to best hand(s) among its eligibles.
void resolve_showdown(Engine& E, int pot_unused) {
    (void)pot_unused;
    // gather contributions
    std::vector<int> contrib(E.N, 0);
    for (int i = 0; i < E.N; ++i) contrib[i] = E.P[i].total_committed;

    // evaluate hands for non-folded players
    std::vector<uint32_t> score(E.N, 0);
    std::vector<bool> contender(E.N, false);
    for (int i = 0; i < E.N; ++i) {
        if (!E.P[i].folded && !E.P[i].eliminated) {
            std::array<int,7> h{ E.P[i].hole[0], E.P[i].hole[1],
                E.board[0],E.board[1],E.board[2],E.board[3],E.board[4] };
            score[i] = eval7(h);
            contender[i] = true;
        }
    }

    // broadcast SHOWDOWN lines if >1 contender
    int n_cont = 0; for (int i=0;i<E.N;++i) if (contender[i]) ++n_cont;
    if (n_cont > 1) {
        for (int i = 0; i < E.N; ++i)
            if (contender[i])
                E.broadcast("SHOWDOWN " + std::to_string(i) + " " +
                            card_str(E.P[i].hole[0]) + " " + card_str(E.P[i].hole[1]));
    }

    // peel layers
    while (true) {
        // smallest positive contribution among anyone who still has chips in
        int layer = INT32_MAX;
        for (int i = 0; i < E.N; ++i) if (contrib[i] > 0) layer = std::min(layer, contrib[i]);
        if (layer == INT32_MAX) break; // all pots distributed

        int pot_amt = 0;
        std::vector<int> in_layer;
        for (int i = 0; i < E.N; ++i) {
            if (contrib[i] > 0) {
                pot_amt += layer;
                contrib[i] -= layer;
                if (contender[i]) in_layer.push_back(i);
            }
        }
        if (in_layer.empty()) {
            // everyone who paid this layer folded; award to remaining contenders
            // (shouldn't usually happen, but handle gracefully)
            for (int i = 0; i < E.N; ++i) if (contender[i]) { in_layer.push_back(i); }
            if (in_layer.empty()) continue;
        }
        // best score among in_layer
        uint32_t best = 0;
        for (int i : in_layer) best = std::max(best, score[i]);
        std::vector<int> winners;
        for (int i : in_layer) if (score[i] == best) winners.push_back(i);

        int share = pot_amt / (int)winners.size();
        int rem = pot_amt % (int)winners.size();
        // remainder to player closest left of dealer -> approximate: lowest seat
        for (size_t w = 0; w < winners.size(); ++w) {
            int s = winners[w];
            int amt = share + (w < (size_t)rem ? 1 : 0);
            E.P[s].chips += amt;
            E.broadcast("WINNER " + std::to_string(s) + " " + std::to_string(amt) + " " +
                        rank_name(score[s]));
        }
    }
}

// ---- single hand ----
void play_hand(Engine& E, int hand_no, int dealer) {
    // reset per-hand state
    for (int i = 0; i < E.N; ++i) {
        E.P[i].folded = E.P[i].eliminated;
        E.P[i].allin = false;
        E.P[i].committed = 0;
        E.P[i].total_committed = 0;
        E.P[i].errored_phase = false;
        E.P[i].hole[0] = E.P[i].hole[1] = -1;
    }
    E.board.clear();
    E.shuffle_deck();

    // blind seats
    auto next_alive = [&](int s){ for (int k=1;k<=E.N;++k){int t=(s+k)%E.N; if(!E.P[t].eliminated) return t;} return s; };
    int alive = E.count_alive();
    int sb_seat, bb_seat;
    if (alive == 2) { sb_seat = dealer; bb_seat = next_alive(dealer); }
    else { sb_seat = next_alive(dealer); bb_seat = next_alive(sb_seat); }
    int sb_amt = E.small_blind, bb_amt = 2 * E.small_blind;

    E.broadcast("HAND_START " + std::to_string(hand_no) + " " + std::to_string(dealer) + " " +
                std::to_string(sb_seat) + " " + std::to_string(bb_seat) + " " +
                std::to_string(sb_amt) + " " + std::to_string(bb_amt));
    { std::ostringstream c; c << "CHIPS"; for (int i=0;i<E.N;++i) c << " " << E.P[i].chips; E.broadcast(c.str()); }

    // deal holes
    for (int i = 0; i < E.N; ++i) {
        if (E.P[i].eliminated) continue;
        E.P[i].hole[0] = E.draw(); E.P[i].hole[1] = E.draw();
        E.tell(i, "DEAL_HOLE " + card_str(E.P[i].hole[0]) + " " + card_str(E.P[i].hole[1]));
    }

    int pot = 0;
    // post blinds
    auto post = [&](int seat, int amt){ int pay=std::min(amt,E.P[seat].chips); E.P[seat].chips-=pay; E.P[seat].committed=pay; E.P[seat].total_committed=pay; if(E.P[seat].chips==0) E.P[seat].allin=true; };
    post(sb_seat, sb_amt); post(bb_seat, bb_amt);

    // pre-flop swap (street 0) -- happens before betting per RULES flow
    swap_phase_impl(E, 0, pot);

    // pre-flop betting: first to act = left of BB
    int first = next_alive(bb_seat);
    E.betting_round(first, bb_amt, pot);

    // flop / turn / river
    const char* deal_names[3] = {"DEAL_FLOP","DEAL_TURN","DEAL_RIVER"};
    int street_cards[3] = {3,1,1};
    for (int st = 0; st < 3; ++st) {
        if (E.count_active() <= 1) break;
        int prior = (int)E.board.size();
        std::string msg = deal_names[st];
        for (int c = 0; c < street_cards[st]; ++c) { int card=E.draw(); E.board.push_back(card); msg += " " + card_str(card); }
        E.broadcast(msg);

        swap_phase_impl(E, st + 1, pot);
        if (E.count_active() > 1) vote_phase_impl(E, st + 1, prior, pot);

        if (E.count_active() > 1) {
            int fa = next_alive(dealer);
            // ensure fa can act
            for (int k=0;k<E.N && (E.P[fa].folded||E.P[fa].eliminated||E.P[fa].allin); ++k) fa = next_alive(fa);
            E.betting_round(fa, 0, pot);
        }
    }

    // showdown / award
    int active = E.count_active();
    if (active == 1) {
        // last player standing takes the pot (sum of all total_committed)
        int winner = -1; for (int i=0;i<E.N;++i) if (!E.P[i].folded && !E.P[i].eliminated) winner=i;
        int total = 0; for (int i=0;i<E.N;++i) total += E.P[i].total_committed;
        E.P[winner].chips += total;
    } else if (active == 0) {
        // all-error split rule (simplified): split among players who errored in
        // the most recent phase, else just refund total_committed to last-actives.
        std::vector<int> err; for (int i=0;i<E.N;++i) if (E.P[i].errored_phase) err.push_back(i);
        int total = 0; for (int i=0;i<E.N;++i) total += E.P[i].total_committed;
        if (!err.empty()) { int sh=total/(int)err.size(); for(int s:err) E.P[s].chips+=sh; }
    } else {
        resolve_showdown(E, pot);
    }
}

int Engine::main_loop() {
    Engine& E = *this;
    int dealer = 0;
    int revolutions = 0;
    int hand_no = 0;
    int starting_dealer = 0;

    // send GAME_START
    for (int i = 0; i < E.N; ++i) {
        if (E.P[i].eliminated) continue;
        std::ostringstream g;
        g << "GAME_START " << E.N << " " << i << " " << E.starting_chips << " "
          << E.swap_mult[0] << " " << E.swap_mult[1] << " " << E.swap_mult[2] << " " << E.swap_mult[3];
        E.bots[i].send(g.str());
    }

    while (E.count_alive() > 1 && revolutions < 200) {
        // eliminate zero-chip players at hand start
        for (int i = 0; i < E.N; ++i)
            if (!E.P[i].eliminated && E.P[i].chips <= 0) { E.P[i].eliminated = true; E.broadcast("ELIMINATE " + std::to_string(i)); }
        if (E.count_alive() <= 1) break;

        // advance dealer to next alive seat
        for (int k=1;k<=E.N;++k){int t=(dealer+k)%E.N; if(!E.P[t].eliminated){dealer=t;break;}}
        if (dealer == starting_dealer) {
            ++revolutions;
            // blind escalation (RULES 2.3)
            int min_stack = INT32_MAX;
            for (int i=0;i<E.N;++i) if (!E.P[i].eliminated) min_stack = std::min(min_stack, E.P[i].chips);
            int ceil_third = (min_stack + 2) / 3;
            int doubled = 2 * E.small_blind;
            E.small_blind = std::max(E.small_blind, std::min(doubled, ceil_third));
        }

        ++hand_no;
        play_hand(E, hand_no, dealer);
    }

    // determine winner
    int alive = E.count_alive();
    if (alive == 1) {
        for (int i=0;i<E.N;++i) if (!E.P[i].eliminated) return i;
    }
    // revolution limit: most chips wins; ties -> return -1 (rig treats as tie)
    int best = -1, bestc = -1; bool tie=false;
    for (int i=0;i<E.N;++i) if (!E.P[i].eliminated) {
        if (E.P[i].chips > bestc) { bestc=E.P[i].chips; best=i; tie=false; }
        else if (E.P[i].chips == bestc) tie=true;
    }
    return tie ? -1 : best;
}
