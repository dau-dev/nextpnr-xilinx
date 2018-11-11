/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <cmath>
#include <queue>

#include "log.h"
#include "router1.h"
#include "timing.h"

namespace {

USING_NEXTPNR_NAMESPACE

struct arc_key
{
    NetInfo *net_info;
    int user_idx;

    bool operator==(const arc_key &other) const {
        return (net_info == other.net_info) && (user_idx == other.user_idx);
    }

    struct Hash
    {
        std::size_t operator()(const arc_key &arg) const noexcept
        {
            std::size_t seed = std::hash<NetInfo*>()(arg.net_info);
            seed ^= std::hash<int>()(arg.user_idx) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
};

struct arc_entry
{
    arc_key arc;
    delay_t pri;

    struct Greater
    {
        bool operator()(const arc_entry &lhs, const arc_entry &rhs) const noexcept
        {
            return lhs.pri > rhs.pri;
        }
    };
};

struct QueuedWire
{
    WireId wire;
    PipId pip;

    delay_t delay = 0, penalty = 0, bonus = 0, togo = 0;
    int randtag = 0;

    struct Greater
    {
        bool operator()(const QueuedWire &lhs, const QueuedWire &rhs) const noexcept
        {
            delay_t l = lhs.delay + lhs.penalty + lhs.togo;
            delay_t r = rhs.delay + rhs.penalty + rhs.togo;
            NPNR_ASSERT(l >= 0);
            NPNR_ASSERT(r >= 0);
            l -= lhs.bonus;
            r -= rhs.bonus;
            return l == r ? lhs.randtag > rhs.randtag : l > r;
        }
    };
};

struct Router1
{
    Context *ctx;
    const Router1Cfg &cfg;

    std::priority_queue<arc_entry, std::vector<arc_entry>, arc_entry::Greater> arc_queue;
    std::unordered_map<WireId, std::unordered_set<arc_key, arc_key::Hash>> wire_to_arcs;
    std::unordered_map<arc_key, std::unordered_set<WireId>, arc_key::Hash> arc_to_wires;
    std::unordered_set<arc_key, arc_key::Hash> queued_arcs;

    std::unordered_map<WireId, QueuedWire> visited;
    std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> queue;

    std::unordered_map<WireId, int> wireScores;
    std::unordered_map<NetInfo*, int> netScores;

    int arcs_with_ripup = 0;
    int arcs_without_ripup = 0;
    bool ripup_flag;

    Router1(Context *ctx, const Router1Cfg &cfg) : ctx(ctx), cfg(cfg) { }

    void arc_queue_insert(const arc_key &arc, WireId src_wire, WireId dst_wire)
    {
        if (queued_arcs.count(arc))
            return;

        delay_t pri = ctx->estimateDelay(src_wire, dst_wire) - arc.net_info->users[arc.user_idx].budget;

        arc_entry entry;
        entry.arc = arc;
        entry.pri = pri;

        arc_queue.push(entry);
        queued_arcs.insert(arc);
    }

    void arc_queue_insert(const arc_key &arc)
    {
        if (queued_arcs.count(arc))
            return;

        NetInfo *net_info = arc.net_info;
        int user_idx = arc.user_idx;

        auto src_wire = ctx->getNetinfoSourceWire(net_info);
        auto dst_wire = ctx->getNetinfoSinkWire(net_info, net_info->users[user_idx]);

        arc_queue_insert(arc, src_wire, dst_wire);
    }

    arc_key arc_queue_pop()
    {
        arc_entry entry = arc_queue.top();
        arc_queue.pop();
        queued_arcs.erase(entry.arc);
        return entry.arc;
    }

    void ripup_net(NetInfo *net)
    {
        if (ctx->debug)
            log("      ripup net %s\n", net->name.c_str(ctx));

        netScores[net]++;

        auto net_wires_copy = net->wires;
        for (auto &it : net_wires_copy) {
            WireId w = it.first;

            for (auto &it : wire_to_arcs[w]) {
                arc_to_wires[it].erase(w);
                arc_queue_insert(it);
            }
            wire_to_arcs[w].clear();

            if (ctx->debug)
                log("        unbind wire %s\n", ctx->getWireName(w).c_str(ctx));

            ctx->unbindWire(w);
            wireScores[w]++;
        }

        ripup_flag = true;
    }

    void ripup_wire(WireId wire, int extra_indent = 0)
    {
        if (ctx->debug)
            log("    ripup wire %s\n", ctx->getWireName(wire).c_str(ctx));

        WireId w = ctx->getConflictingWireWire(wire);

        if (w == WireId()) {
            NetInfo *n = ctx->getConflictingWireNet(wire);
            if (n != nullptr)
                ripup_net(n);
        } else {
            for (auto &it : wire_to_arcs[w]) {
                arc_to_wires[it].erase(w);
                arc_queue_insert(it);
            }
            wire_to_arcs[w].clear();

            if (ctx->debug)
                log("      unbind wire %s\n", ctx->getWireName(w).c_str(ctx));

            ctx->unbindWire(w);
            wireScores[w]++;
        }

        ripup_flag = true;
    }

    void ripup_pip(PipId pip)
    {
        if (ctx->debug)
            log("    ripup pip %s\n", ctx->getPipName(pip).c_str(ctx));

        WireId w = ctx->getConflictingPipWire(pip);

        if (w == WireId()) {
            NetInfo *n = ctx->getConflictingPipNet(pip);
            if (n != nullptr)
                ripup_net(n);
        } else {
            for (auto &it : wire_to_arcs[w]) {
                arc_to_wires[it].erase(w);
                arc_queue_insert(it);
            }
            wire_to_arcs[w].clear();

            if (ctx->debug)
                log("      unbind wire %s\n", ctx->getWireName(w).c_str(ctx));

            ctx->unbindWire(w);
            wireScores[w]++;
        }

        ripup_flag = true;
    }

    bool skip_net(NetInfo *net_info)
    {
#ifdef ARCH_ECP5
        // ECP5 global nets currently appear part-unrouted due to arch database limitations
        // Don't touch them in the router
        if (net_info->is_global)
            return true;
#endif
        if (net_info->driver.cell == nullptr)
            return true;

        return false;
    }

    void check()
    {
        std::unordered_set<arc_key, arc_key::Hash> valid_arcs;

        for (auto &net_it : ctx->nets)
        {
            NetInfo *net_info = net_it.second.get();
            std::unordered_set<WireId> valid_wires_for_net;

            if (skip_net(net_info))
                continue;

            // log("[check] net: %s\n", net_info->name.c_str(ctx));

            auto src_wire = ctx->getNetinfoSourceWire(net_info);
            log_assert(src_wire != WireId());

            for (int user_idx = 0; user_idx < int(net_info->users.size()); user_idx++) {
                auto dst_wire = ctx->getNetinfoSinkWire(net_info, net_info->users[user_idx]);
                log_assert(dst_wire != WireId());

                arc_key arc;
                arc.net_info = net_info;
                arc.user_idx = user_idx;

                valid_arcs.insert(arc);
                // log("[check]   arc: %s %s\n", ctx->getWireName(src_wire).c_str(ctx), ctx->getWireName(dst_wire).c_str(ctx));

                for (WireId wire : arc_to_wires[arc]) {
                    // log("[check]     wire: %s\n", ctx->getWireName(wire).c_str(ctx));
                    valid_wires_for_net.insert(wire);
                    log_assert(wire_to_arcs[wire].count(arc));
                    log_assert(net_info->wires.count(wire));
                }
            }

            for (auto &it : net_info->wires) {
                WireId w = it.first;
                log_assert(valid_wires_for_net.count(w));
            }
        }

        for (auto &it : wire_to_arcs) {
            for (auto &arc : it.second)
                log_assert(valid_arcs.count(arc));
        }

        for (auto &it : arc_to_wires) {
            log_assert(valid_arcs.count(it.first));
        }
    }

    void setup()
    {
        std::unordered_map<WireId, NetInfo*> src_to_net;
        std::unordered_map<WireId, arc_key> dst_to_arc;

        for (auto &net_it : ctx->nets)
        {
            NetInfo *net_info = net_it.second.get();

            if (skip_net(net_info))
                continue;

            auto src_wire = ctx->getNetinfoSourceWire(net_info);

            if (src_wire == WireId())
                log_error("No wire found for port %s on source cell %s.\n", net_info->driver.port.c_str(ctx),
                          net_info->driver.cell->name.c_str(ctx));

            if (src_to_net.count(src_wire))
                log_error("Found two nets with same source wire %s: %s vs %s\n", ctx->getWireName(src_wire).c_str(ctx),
                          ctx->nameOf(net_info), ctx->nameOf(src_to_net.at(src_wire)));

            if (dst_to_arc.count(src_wire))
                log_error("Wire %s is used as source and sink in different nets: %s vs %s (%d)\n", ctx->getWireName(src_wire).c_str(ctx),
                          ctx->nameOf(net_info), ctx->nameOf(dst_to_arc.at(src_wire).net_info), dst_to_arc.at(src_wire).user_idx);

            for (int user_idx = 0; user_idx < int(net_info->users.size()); user_idx++) {
                auto dst_wire = ctx->getNetinfoSinkWire(net_info, net_info->users[user_idx]);

                if (dst_wire == WireId())
                    log_error("No wire found for port %s on destination cell %s.\n", net_info->users[user_idx].port.c_str(ctx),
                              net_info->users[user_idx].cell->name.c_str(ctx));

                if (dst_to_arc.count(dst_wire)) {
                    if (dst_to_arc.at(dst_wire).net_info == net_info)
                        continue;
                    log_error("Found two arcs with same sink wire %s: %s (%d) vs %s (%d)\n", ctx->getWireName(dst_wire).c_str(ctx),
                              ctx->nameOf(net_info), user_idx, ctx->nameOf(dst_to_arc.at(dst_wire).net_info), dst_to_arc.at(dst_wire).user_idx);
                }

                if (src_to_net.count(dst_wire))
                    log_error("Wire %s is used as source and sink in different nets: %s vs %s (%d)\n", ctx->getWireName(dst_wire).c_str(ctx),
                              ctx->nameOf(src_to_net.at(dst_wire)), ctx->nameOf(net_info), user_idx);

                arc_key arc;
                arc.net_info = net_info;
                arc.user_idx = user_idx;

                dst_to_arc[dst_wire] = arc;

                if (net_info->wires.count(src_wire) == 0) {
                    arc_queue_insert(arc, src_wire, dst_wire);
                    continue;
                }

                WireId cursor = dst_wire;
                wire_to_arcs[cursor].insert(arc);
                arc_to_wires[arc].insert(cursor);

                while (src_wire != cursor) {
                    auto it = net_info->wires.find(cursor);
                    if (it == net_info->wires.end()) {
                        arc_queue_insert(arc, src_wire, dst_wire);
                        break;
                    }

                    NPNR_ASSERT(it->second.pip != PipId());
                    cursor = ctx->getPipSrcWire(it->second.pip);
                    wire_to_arcs[cursor].insert(arc);
                    arc_to_wires[arc].insert(cursor);
                }
            }

            src_to_net[src_wire] = net_info;

            std::vector<WireId> unbind_wires;

            for (auto &it : net_info->wires)
                if (it.second.strength < STRENGTH_LOCKED && wire_to_arcs.count(it.first) == 0)
                    unbind_wires.push_back(it.first);

            for (auto it : unbind_wires)
                ctx->unbindWire(it);
        }
    }

    bool route_arc(const arc_key &arc, bool ripup)
    {

        NetInfo *net_info = arc.net_info;
        int user_idx = arc.user_idx;

        auto src_wire = ctx->getNetinfoSourceWire(net_info);
        auto dst_wire = ctx->getNetinfoSinkWire(net_info, net_info->users[user_idx]);
        ripup_flag = false;

        if (ctx->debug) {
            log("Routing arc %d on net %s (%d arcs total):\n", user_idx, net_info->name.c_str(ctx), int(net_info->users.size()));
            log("  source ... %s\n", ctx->getWireName(src_wire).c_str(ctx));
            log("  sink ..... %s\n", ctx->getWireName(dst_wire).c_str(ctx));
        }

        // unbind wires that are currently used exclusively by this arc

        std::unordered_set<WireId> old_arc_wires;
        old_arc_wires.swap(arc_to_wires[arc]);

        for (WireId wire : old_arc_wires) {
            auto &arc_wires = wire_to_arcs.at(wire);
            NPNR_ASSERT(arc_wires.count(arc));
            arc_wires.erase(arc);
            if (arc_wires.empty()) {
                if (ctx->debug)
                    log("  unbind %s\n", ctx->getWireName(wire).c_str(ctx));
                ctx->unbindWire(wire);
            }
        }

        // reset wire queue

        if (!queue.empty()) {
            std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> new_queue;
            queue.swap(new_queue);
        }
        visited.clear();

        // A* main loop

        int visitCnt = 0;
        int maxVisitCnt = INT_MAX;
        delay_t best_est = 0;
        delay_t best_score = -1;

        {
            QueuedWire qw;
            qw.wire = src_wire;
            qw.pip = PipId();
            qw.delay = ctx->getWireDelay(qw.wire).maxDelay();
            qw.penalty = 0;
            qw.bonus = 0;
            if (cfg.useEstimate) {
                qw.togo = ctx->estimateDelay(qw.wire, dst_wire);
                best_est = qw.delay + qw.togo;
            }
            qw.randtag = ctx->rng();

            queue.push(qw);
            visited[qw.wire] = qw;
        }

        while (visitCnt++ < maxVisitCnt && !queue.empty())
        {
            QueuedWire qw = queue.top();
            queue.pop();

            for (auto pip : ctx->getPipsDownhill(qw.wire)) {
                delay_t next_delay = qw.delay + ctx->getPipDelay(pip).maxDelay();
                delay_t next_penalty = qw.penalty;
                delay_t next_bonus = qw.bonus;

                WireId next_wire = ctx->getPipDstWire(pip);
                next_delay += ctx->getWireDelay(next_wire).maxDelay();

                WireId conflictWireWire = WireId(), conflictPipWire = WireId();
                NetInfo *conflictWireNet = nullptr, *conflictPipNet = nullptr;

                bool wire_reuse = net_info->wires.count(next_wire);
                bool pip_reuse = wire_reuse && net_info->wires.at(next_wire).pip == pip;

                if (!ctx->checkWireAvail(next_wire) && !wire_reuse) {
                    if (!ripup)
                        continue;
                    conflictWireWire = ctx->getConflictingWireWire(next_wire);
                    if (conflictWireWire == WireId()) {
                        conflictWireNet = ctx->getConflictingWireNet(next_wire);
                        if (conflictWireNet == nullptr)
                            continue;
                    }
                }

                if (!ctx->checkPipAvail(pip) && !pip_reuse) {
                    if (!ripup)
                        continue;
                    conflictPipWire = ctx->getConflictingPipWire(pip);
                    if (conflictPipWire == WireId()) {
                        conflictPipNet = ctx->getConflictingPipNet(pip);
                        if (conflictPipNet == nullptr)
                            continue;
                    }
                }

                if (conflictWireNet != nullptr && conflictPipWire != WireId() && conflictWireNet->wires.count(conflictPipWire))
                    conflictPipWire = WireId();

                if (conflictPipNet != nullptr && conflictWireWire != WireId() && conflictPipNet->wires.count(conflictWireWire))
                    conflictWireWire = WireId();

                if (conflictWireWire == conflictPipWire)
                    conflictWireWire = WireId();

                if (conflictWireNet == conflictPipNet)
                    conflictWireNet = nullptr;

                if (wire_reuse)
                    next_bonus += cfg.wireReuseBonus;

                if (pip_reuse)
                    next_bonus += cfg.pipReuseBonus;

                if (conflictWireWire != WireId()) {
                    auto scores_it = wireScores.find(conflictWireWire);
                    if (scores_it != wireScores.end())
                        next_penalty += scores_it->second * cfg.wireRipupPenalty;
                    next_penalty += cfg.wireRipupPenalty;
                }

                if (conflictPipWire != WireId()) {
                    auto scores_it = wireScores.find(conflictPipWire);
                    if (scores_it != wireScores.end())
                        next_penalty += scores_it->second * cfg.wireRipupPenalty;
                    next_penalty += cfg.wireRipupPenalty;
                }

                if (conflictWireNet != nullptr) {
                    auto scores_it = netScores.find(conflictWireNet);
                    if (scores_it != netScores.end())
                        next_penalty += scores_it->second * cfg.netRipupPenalty;
                    next_penalty += cfg.netRipupPenalty;
                    next_penalty += conflictWireNet->wires.size() * cfg.wireRipupPenalty;
                }

                if (conflictPipNet != nullptr) {
                    auto scores_it = netScores.find(conflictPipNet);
                    if (scores_it != netScores.end())
                        next_penalty += scores_it->second * cfg.netRipupPenalty;
                    next_penalty += cfg.netRipupPenalty;
                    next_penalty += conflictPipNet->wires.size() * cfg.wireRipupPenalty;
                }

                delay_t next_score = next_delay + next_penalty;
                NPNR_ASSERT(next_score >= 0);

                if ((best_score >= 0) && (next_score - next_bonus - cfg.estimatePrecision > best_score))
                    continue;

                auto old_visited_it = visited.find(next_wire);
                if (old_visited_it != visited.end()) {
                    delay_t old_delay = old_visited_it->second.delay;
                    delay_t old_score = old_delay + old_visited_it->second.penalty;
                    NPNR_ASSERT(old_score >= 0);

                    if (next_score + ctx->getDelayEpsilon() >= old_score)
                        continue;

#if 0
                    if (ctx->debug)
                        log("Found better route to %s. Old vs new delay estimate: %.3f (%.3f) %.3f (%.3f)\n",
                            ctx->getWireName(next_wire).c_str(ctx),
                            ctx->getDelayNS(old_score),
                            ctx->getDelayNS(old_visited_it->second.delay),
                            ctx->getDelayNS(next_score),
                            ctx->getDelayNS(next_delay));
#endif
                }

                QueuedWire next_qw;
                next_qw.wire = next_wire;
                next_qw.pip = pip;
                next_qw.delay = next_delay;
                next_qw.penalty = next_penalty;
                next_qw.bonus = next_bonus;
                if (cfg.useEstimate) {
                    next_qw.togo = ctx->estimateDelay(next_wire, dst_wire);
                    delay_t this_est = next_qw.delay + next_qw.togo;
                    if (this_est/2 - cfg.estimatePrecision > best_est)
                        continue;
                    if (best_est > this_est)
                        best_est = this_est;
                }
                next_qw.randtag = ctx->rng();

#if 0
                if (ctx->debug)
                    log("%s -> %s: %.3f (%.3f)\n",
                        ctx->getWireName(qw.wire).c_str(ctx),
                        ctx->getWireName(next_wire).c_str(ctx),
                        ctx->getDelayNS(next_score),
                        ctx->getDelayNS(next_delay));
#endif

                visited[next_qw.wire] = next_qw;
                queue.push(next_qw);

                if (next_wire == dst_wire) {
                    if (maxVisitCnt == INT_MAX)
                        maxVisitCnt = 2*visitCnt;
                    best_score = next_score - next_bonus;
                }
            }
        }

        if (ctx->debug)
            log("  total number of visited nodes: %d\n", visitCnt);

        if (visited.count(dst_wire) == 0) {
            if (ctx->debug)
                log("  no route found for this arc\n");
            return false;
        }

        if (ctx->debug) {
            log("  final route delay:   %8.2f\n", ctx->getDelayNS(visited[dst_wire].delay));
            log("  final route penalty: %8.2f\n", ctx->getDelayNS(visited[dst_wire].penalty));
            log("  final route bonus:   %8.2f\n", ctx->getDelayNS(visited[dst_wire].bonus));
        }

        // bind resulting route (and maybe unroute other nets)

        std::unordered_set<WireId> unassign_wires = arc_to_wires[arc];

        WireId cursor = dst_wire;
        while (1) {
            auto pip = visited[cursor].pip;

            if (ctx->debug)
                log("  node %s\n", ctx->getWireName(cursor).c_str(ctx));

                if (pip == PipId())
                    NPNR_ASSERT(cursor == src_wire);

            if (!net_info->wires.count(cursor) || net_info->wires.at(cursor).pip != pip) {
                if (!ctx->checkWireAvail(cursor)) {
                    ripup_wire(cursor);
                    NPNR_ASSERT(ctx->checkWireAvail(cursor));
                }

                if (pip != PipId() && !ctx->checkPipAvail(pip)) {
                    ripup_pip(pip);
                    NPNR_ASSERT(ctx->checkPipAvail(pip));
                }

                if (pip == PipId()) {
                    if (ctx->debug)
                        log("    bind wire %s\n", ctx->getWireName(cursor).c_str(ctx));
                    ctx->bindWire(cursor, net_info, STRENGTH_WEAK);
                } else {
                    if (ctx->debug)
                        log("    bind pip %s\n", ctx->getPipName(pip).c_str(ctx));
                    ctx->bindPip(pip, net_info, STRENGTH_WEAK);
                }
            }

            wire_to_arcs[cursor].insert(arc);
            arc_to_wires[arc].insert(cursor);

            if (pip == PipId())
                break;

            cursor = ctx->getPipSrcWire(pip);
        }

        if (ripup_flag)
            arcs_with_ripup++;
        else
            arcs_without_ripup++;

        return true;
    }
};

} // namespace

NEXTPNR_NAMESPACE_BEGIN

Router1Cfg::Router1Cfg(Context *ctx) : Settings(ctx)
{
    maxIterCnt = get<int>("router1/maxIterCnt", 200);
    cleanupReroute = get<bool>("router1/cleanupReroute", true);
    fullCleanupReroute = get<bool>("router1/fullCleanupReroute", true);
    useEstimate = get<bool>("router1/useEstimate", true);

    wireRipupPenalty = ctx->getRipupDelayPenalty();
    netRipupPenalty = 10*ctx->getRipupDelayPenalty();

    wireReuseBonus = wireRipupPenalty/8;
    pipReuseBonus = wireRipupPenalty/2;

    estimatePrecision = 100 * ctx->getRipupDelayPenalty();
}

bool router1(Context *ctx, const Router1Cfg &cfg)
{
    try {
        log_break();
        log_info("Routing..\n");
        ctx->lock();

        log_info("Setting up routing queue.\n");

        Router1 router(ctx, cfg);
        router.setup();
#ifndef NDEBUG
        router.check();
#endif

        log_info("Routing %d arcs.\n", int(router.arc_queue.size()));

        int iter_cnt = 0;
        int last_arcs_with_ripup = 0;
        int last_arcs_without_ripup = 0;

        log_info("           |   (re-)routed arcs  |   delta    | remaining\n");
        log_info("   IterCnt |  w/ripup   wo/ripup |  w/r  wo/r |      arcs\n");

        while (!router.arc_queue.empty()) {
            if (++iter_cnt % 1000 == 0) {
                log_info("%10d | %8d %10d | %4d %5d | %9d\n",
                        iter_cnt, router.arcs_with_ripup, router.arcs_without_ripup,
                        router.arcs_with_ripup - last_arcs_with_ripup,
                        router.arcs_without_ripup - last_arcs_without_ripup, int(router.arc_queue.size()));
                last_arcs_with_ripup = router.arcs_with_ripup;
                last_arcs_without_ripup = router.arcs_without_ripup;
#ifndef NDEBUG
                router.check();
#endif
            }

            if (ctx->debug)
                log("-- %d --\n", iter_cnt);

            arc_key arc = router.arc_queue_pop();

            if (!router.route_arc(arc, true)) {
                log_warning("Failed to find a route for arc %d of net %s.\n",
                        arc.user_idx, arc.net_info->name.c_str(ctx));
#ifndef NDEBUG
                router.check();
                ctx->check();
#endif
                ctx->unlock();
                return false;
            }
        }

        log_info("%10d | %8d %10d | %4d %5d | %9d\n",
                iter_cnt, router.arcs_with_ripup, router.arcs_without_ripup,
                router.arcs_with_ripup - last_arcs_with_ripup,
                router.arcs_without_ripup - last_arcs_without_ripup, int(router.arc_queue.size()));
        log_info("Routing complete.\n");

#ifndef NDEBUG
        router.check();
        ctx->check();
        log_assert(ctx->checkRoutedDesign());
#endif

        log_info("Checksum: 0x%08x\n", ctx->checksum());
        timing_analysis(ctx, true /* slack_histogram */, true /* print_path */);

        ctx->unlock();
        return true;
    } catch (log_execution_error_exception) {
#ifndef NDEBUG
        ctx->check();
#endif
        ctx->unlock();
        return false;
    }
}

bool Context::checkRoutedDesign() const
{
        const Context *ctx = getCtx();

        for (auto &net_it : ctx->nets) {
            NetInfo *net_info = net_it.second.get();

            if (ctx->debug)
                log("checking net %s\n", net_info->name.c_str(ctx));

            if (net_info->users.empty()) {
                if (ctx->debug)
                    log("  net without sinks\n");
                log_assert(net_info->wires.empty());
                continue;
            }

            bool found_unrouted = false;
            bool found_loop = false;
            bool found_stub = false;

            struct ExtraWireInfo {
                int order_num = 0;
                std::unordered_set<WireId> children;
            };

            std::unordered_map<WireId, ExtraWireInfo> db;

            for (auto &it : net_info->wires) {
                WireId w = it.first;
                PipId p = it.second.pip;

                if (p != PipId()) {
                    log_assert(ctx->getPipDstWire(p) == w);
                    db[ctx->getPipSrcWire(p)].children.insert(w);
                }
            }

            auto src_wire = ctx->getNetinfoSourceWire(net_info);
            log_assert(src_wire != WireId());

            if (net_info->wires.count(src_wire) == 0) {
                if (ctx->debug)
                    log("  source (%s) not bound to net\n", ctx->getWireName(src_wire).c_str(ctx));
                found_unrouted = true;
            }

            std::unordered_map<WireId, int> dest_wires;
            for (int user_idx = 0; user_idx < int(net_info->users.size()); user_idx++) {
                auto dst_wire = ctx->getNetinfoSinkWire(net_info, net_info->users[user_idx]);
                log_assert(dst_wire != WireId());
                dest_wires[dst_wire] = user_idx;

                if (net_info->wires.count(dst_wire) == 0) {
                    if (ctx->debug)
                        log("  sink %d (%s) not bound to net\n", user_idx, ctx->getWireName(dst_wire).c_str(ctx));
                    found_unrouted = true;
                }
            }

            std::function<void(WireId, int)> setOrderNum;
            std::unordered_set<WireId> logged_wires;

            setOrderNum = [&](WireId w, int num) {
                auto &db_entry = db[w];
                if (db_entry.order_num != 0) {
                    found_loop = true;
                    log("  %*s=> loop\n", 2*num, "");
                    return;
                }
                db_entry.order_num = num;
                for (WireId child : db_entry.children) {
                    if (ctx->debug) {
                        log("  %*s-> %s\n", 2*num, "", ctx->getWireName(child).c_str(ctx));
                        logged_wires.insert(child);
                    }
                    setOrderNum(child, num+1);
                }
                if (db_entry.children.empty()) {
                    if (dest_wires.count(w) != 0) {
                        if (ctx->debug)
                            log("  %*s=> sink %d\n", 2*num, "", dest_wires.at(w));
                    } else {
                        if (ctx->debug)
                            log("  %*s=> stub\n", 2*num, "");
                        found_stub = true;
                    }
                }
            };

            if (ctx->debug) {
                log("  driver: %s\n", ctx->getWireName(src_wire).c_str(ctx));
                logged_wires.insert(src_wire);
            }
            setOrderNum(src_wire, 1);

            std::unordered_set<WireId> dangling_wires;

            for (auto &it : db) {
                auto &db_entry = it.second;
                if (db_entry.order_num == 0)
                    dangling_wires.insert(it.first);
            }

            if (ctx->debug) {
                if (dangling_wires.empty()) {
                    log("  no dangling wires.\n");
                } else {
                    std::unordered_set<WireId> root_wires = dangling_wires;

                    for (WireId w : dangling_wires) {
                        for (WireId c : db[w].children)
                            root_wires.erase(c);
                    }

                    for (WireId w : root_wires) {
                        log("  dangling wire: %s\n", ctx->getWireName(w).c_str(ctx));
                        logged_wires.insert(w);
                        setOrderNum(w, 1);
                    }

                    for (WireId w : dangling_wires) {
                        if (logged_wires.count(w) == 0)
                            log("  loop: %s -> %s\n",
                                ctx->getWireName(ctx->getPipSrcWire(net_info->wires.at(w).pip)).c_str(ctx),
                                ctx->getWireName(w).c_str(ctx));
                    }
                }
            }

            bool fail = false;

            if (found_unrouted) {
                if (ctx->debug)
                    log("check failed: found unrouted arcs\n");
                fail = true;
            }

            if (found_loop) {
                if (ctx->debug)
                    log("check failed: found loops\n");
                fail = true;
            }

            if (found_stub) {
                if (ctx->debug)
                    log("check failed: found stubs\n");
                fail = true;
            }

            if (!dangling_wires.empty()) {
                if (ctx->debug)
                    log("check failed: found dangling wires\n");
                fail = true;
            }

            if (fail)
                return false;
        }

        return true;
}

bool Context::getActualRouteDelay(WireId src_wire, WireId dst_wire, delay_t *delay,
                                  std::unordered_map<WireId, PipId> *route, bool useEstimate)
{
    // FIXME
    return false;
}

NEXTPNR_NAMESPACE_END
