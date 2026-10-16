#ifndef __MEM_COSMOS_RL_HH__
#define __MEM_COSMOS_RL_HH__

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <list>
#include <random>
#include <unordered_map>
#include <vector>

namespace gem5
{
namespace memory
{
namespace cosmos
{

struct Params
{
    static constexpr unsigned STATES = 16384;

    static constexpr double ALPHA_D = 0.09;
    static constexpr double GAMMA_D = 0.88;
    static constexpr double EPS_D = 0.1;
    static constexpr double R_D_HI = 9;
    static constexpr double R_D_HO = -20;
    static constexpr double R_D_MO = 12;
    static constexpr double R_D_MI = -30;

    static constexpr double ALPHA_C = 0.05;
    static constexpr double GAMMA_C = 0.35;
    static constexpr double EPS_C = 0.001;
    static constexpr double R_C_HG = 13;
    static constexpr double R_C_HB = -12;
    static constexpr double R_C_MG = -16;
    static constexpr double R_C_MB = 20;
    static constexpr double R_C_EG = -22;
    static constexpr double R_C_EB = 26;

    static constexpr unsigned CET_ENTRIES = 8192;
    static constexpr unsigned CET_WINDOW = 32;

    static constexpr double Q_CLAMP = 127.0;
    static constexpr double SCORE_STEP = 16.0;

    static constexpr unsigned DATA_SHIFT = 12;
};

struct Config
{
    unsigned lcrBlocks = 2048;
    unsigned lcrAssoc = 16;
    bool dp = true;
    bool cp = true;
    uint64_t seed = 1;
};

static inline uint64_t
hash64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

class QTable
{
    std::vector<std::array<double, 2>> q;
    unsigned mask = 0;

  public:
    void init(unsigned states)
    {
        assert(states && (states & (states - 1)) == 0);
        q.assign(states, {0.0, 0.0});
        mask = states - 1;
    }
    unsigned size() const { return (unsigned)q.size(); }
    unsigned state(uint64_t h) const { return (unsigned)(h & mask); }
    double get(unsigned s, int a) const { return q[s][a]; }
    double maxQ(unsigned s) const { return std::max(q[s][0], q[s][1]); }
    int argmax(unsigned s) const { return q[s][1] > q[s][0] ? 1 : 0; }

    void update(unsigned s, int a, double r, unsigned s2,
                double alpha, double gamma)
    {
        double &v = q[s][a];
        v += alpha * (r + gamma * maxQ(s2) - v);
        v = std::max(-Params::Q_CLAMP, std::min(Params::Q_CLAMP, v));
    }
};

class Explorer
{
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> uni{0.0, 1.0};

  public:
    void seed(uint64_t s) { rng.seed(s); }
    int choose(const QTable &q, unsigned s, double eps, bool *explored)
    {
        if (eps > 0 && uni(rng) < eps) {
            if (explored) *explored = true;
            return (int)(rng() & 1);
        }
        if (explored) *explored = false;
        return q.argmax(s);
    }
};

class DataPredictor
{
  public:
    enum { ON_CHIP = 0, OFF_CHIP = 1 };

  private:
    QTable q;
    Explorer ex;

  public:
    void init(const Config &c)
    {
        q.init(Params::STATES);
        ex.seed(c.seed ^ 0xD474ULL);
    }

    unsigned stateOf(uint64_t line_addr) const
    {
        return q.state(hash64(line_addr >> Params::DATA_SHIFT));
    }

    int predict(unsigned s, bool *explored = nullptr)
    {
        return ex.choose(q, s, Params::EPS_D, explored);
    }

    double reward(unsigned s, int a, bool on_chip)
    {
        double r;
        if (on_chip)
            r = (a == ON_CHIP) ? Params::R_D_HI : Params::R_D_HO;
        else
            r = (a == OFF_CHIP) ? Params::R_D_MO : Params::R_D_MI;
        q.update(s, a, r, s, Params::ALPHA_D, Params::GAMMA_D);
        return r;
    }

    const QTable &table() const { return q; }
};

class Cet
{
  public:
    struct Ent { uint64_t key; unsigned state; int action; bool hit; };

  private:
    size_t cap = 0;
    std::list<Ent> order;
    std::unordered_map<uint64_t, std::list<Ent>::iterator> pos;

  public:
    void init(size_t c) { cap = c; order.clear(); pos.clear(); }
    size_t size() const { return pos.size(); }
    bool empty() const { return order.empty(); }
    const Ent &head() const { return order.front(); }

    bool lookupNear(uint64_t key, unsigned w)
    {
        const uint64_t lo = (key >= w) ? key - w : 0;
        const uint64_t hi = key + w;
        bool any = false;
        for (uint64_t k = lo; k <= hi; ++k) {
            auto it = pos.find(k);
            if (it != pos.end()) {
                it->second->hit = true;
                any = true;
            }
        }
        return any;
    }

    bool insert(uint64_t key, unsigned state, int action, Ent *ev)
    {
        auto it = pos.find(key);
        if (it != pos.end()) {
            it->second->state = state;
            it->second->action = action;
            order.splice(order.begin(), order, it->second);
            return false;
        }
        bool evicted = false;
        if (cap && pos.size() >= cap) {
            if (ev) *ev = order.back();
            pos.erase(order.back().key);
            order.pop_back();
            evicted = true;
        }
        order.push_front({key, state, action, false});
        pos[key] = order.begin();
        return evicted;
    }
};

class CtrPredictor
{
  public:
    enum { BAD = 0, GOOD = 1 };

    struct Decision
    {
        unsigned state = 0;
        int action = BAD;
        double score = 0;
        bool explored = false;
        bool cetHit = false;
        bool cetEvicted = false;
        double reward = 0;
        double evictReward = 0;
    };

  private:
    QTable q;
    Explorer ex;
    Cet cet;

  public:
    void init(const Config &c)
    {
        q.init(Params::STATES);
        ex.seed(c.seed ^ 0xC7ULL);
        cet.init(Params::CET_ENTRIES);
    }

    unsigned stateOf(uint64_t vn) const { return q.state(hash64(vn)); }

    Decision access(uint64_t vn, uint64_t line)
    {
        Decision d;
        d.state = stateOf(vn);
        d.action = ex.choose(q, d.state, Params::EPS_C, &d.explored);
        const uint64_t key = line >> 6;

        d.cetHit = cet.lookupNear(key, Params::CET_WINDOW);
        if (d.cetHit)
            d.reward = (d.action == GOOD) ? Params::R_C_HG : Params::R_C_HB;
        else
            d.reward = (d.action == GOOD) ? Params::R_C_MG : Params::R_C_MB;

        const unsigned s2 = cet.empty() ? d.state : cet.head().state;
        q.update(d.state, d.action, d.reward, s2,
                 Params::ALPHA_C, Params::GAMMA_C);

        Cet::Ent ev;
        if (cet.insert(key, d.state, d.action, &ev) && !ev.hit) {
            d.cetEvicted = true;
            d.evictReward = (ev.action == GOOD) ? Params::R_C_EG
                                                : Params::R_C_EB;
            q.update(ev.state, ev.action, d.evictReward, s2,
                     Params::ALPHA_C, Params::GAMMA_C);
        }

        d.score = std::floor(q.get(d.state, d.action) / Params::SCORE_STEP)
                  * Params::SCORE_STEP;
        return d;
    }

    const QTable &table() const { return q; }
    const Cet &table_cet() const { return cet; }
};

class LcrCache
{
  public:
    struct Line
    {
        bool valid = false;
        uint64_t vn = 0;
        bool dirty = false;
        bool good = false;
        double score = 0;
        uint64_t stamp = 0;
    };
    struct Victim { bool valid = false; uint64_t vn = 0; bool dirty = false;
                    bool good = false; };

  private:
    std::vector<Line> lines;
    unsigned sets = 0, assoc = 0;
    uint64_t tick = 0;
    bool lcr = true;

    unsigned setOf(uint64_t vn) const { return (unsigned)(vn % sets); }

  public:
    void init(unsigned blocks, unsigned a, bool use_lcr)
    {
        assoc = std::max(1u, std::min(a, blocks));
        sets = std::max(1u, blocks / assoc);
        lines.assign((size_t)sets * assoc, Line());
        lcr = use_lcr;
        tick = 0;
    }
    unsigned numSets() const { return sets; }
    unsigned numWays() const { return assoc; }

    bool probe(uint64_t vn, bool dirty, bool good, double score)
    {
        Line *base = &lines[(size_t)setOf(vn) * assoc];
        for (unsigned w = 0; w < assoc; ++w) {
            Line &l = base[w];
            if (l.valid && l.vn == vn) {
                l.dirty |= dirty;
                l.good = good;
                l.score = score;
                l.stamp = ++tick;
                return true;
            }
        }
        return false;
    }

    unsigned victimWay(unsigned set) const
    {
        const Line *base = &lines[(size_t)set * assoc];
        for (unsigned w = 0; w < assoc; ++w)
            if (!base[w].valid)
                return w;
        if (!lcr) {
            unsigned best = 0;
            for (unsigned w = 1; w < assoc; ++w)
                if (base[w].stamp < base[best].stamp)
                    best = w;
            return best;
        }
        int cand = -1;
        double maxBad = -1e300, minGood = 1e300;
        for (unsigned w = 0; w < assoc; ++w) {
            const Line &l = base[w];
            if (!l.good && (l.score > maxBad ||
                            (l.score == maxBad && cand >= 0 &&
                             l.stamp < base[cand].stamp))) {
                cand = (int)w;
                maxBad = l.score;
            }
        }
        if (cand >= 0)
            return (unsigned)cand;
        for (unsigned w = 0; w < assoc; ++w) {
            const Line &l = base[w];
            if (l.score < minGood ||
                (l.score == minGood && cand >= 0 &&
                 l.stamp < base[cand].stamp)) {
                cand = (int)w;
                minGood = l.score;
            }
        }
        return (unsigned)cand;
    }

    void insert(uint64_t vn, bool dirty, bool good, double score,
                Victim *victim)
    {
        const unsigned set = setOf(vn);
        Line &l = lines[(size_t)set * assoc + victimWay(set)];
        if (victim) {
            victim->valid = l.valid;
            victim->vn = l.vn;
            victim->dirty = l.dirty;
            victim->good = l.good;
        }
        l.valid = true;
        l.vn = vn;
        l.dirty = dirty;
        l.good = good;
        l.score = score;
        l.stamp = ++tick;
    }
};

} // namespace cosmos
} // namespace memory
} // namespace gem5

#endif // __MEM_COSMOS_RL_HH__
