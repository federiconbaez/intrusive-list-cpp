// SPDX-License-Identifier: Apache-2.0
// Queue churn (push_back + pop_front), full iteration and sort: intrusive
// list against std::list<T> (one allocation per push) and std::list<T*>.
// Median of 7 runs, ns per operation.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <list>
#include <random>
#include <vector>

#include "ilist/list.hpp"

struct item : ilist::hook<> {
  std::uint64_t key = 0;
  char payload[48] = {};
};

static volatile std::uint64_t sink;

template <class F>
static double median_ns(F f, double ops) {
  std::vector<double> r;
  for (int i = 0; i < 7; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    auto t1 = std::chrono::steady_clock::now();
    r.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / ops);
  }
  std::sort(r.begin(), r.end());
  return r[3];
}

int main() {
  constexpr std::size_t N = 100'000;
  constexpr int CHURN = 2'000'000;
  std::vector<item> pool(N);
  std::mt19937_64 rng(1);
  for (auto& x : pool) x.key = rng();

  std::printf("%-28s %10s %12s %12s\n", "", "ilist", "std::list<T>", "list<T*>");

  {  // churn on a queue that holds N/2 elements
    ilist::list<item> q;
    std::list<item> sq;
    std::list<item*> pq;
    for (std::size_t i = 0; i < N / 2; ++i) q.push_back(pool[i]), sq.push_back(pool[i]), pq.push_back(&pool[i]);
    double a = median_ns([&] {
      for (int i = 0; i < CHURN; ++i) q.push_back(*q.pop_front());
    }, CHURN);
    double b = median_ns([&] {
      for (int i = 0; i < CHURN; ++i) {
        sq.push_back(sq.front());
        sq.pop_front();
      }
    }, CHURN);
    double c = median_ns([&] {
      for (int i = 0; i < CHURN; ++i) {
        pq.push_back(pq.front());
        pq.pop_front();
      }
    }, CHURN);
    std::printf("%-28s %8.2f ns %10.2f ns %10.2f ns\n", "churn pop_front+push_back", a, b, c);
    q.clear();
  }
  {  // iteration when list order follows memory order
    ilist::list<item> l;
    std::list<item> sl;
    std::list<item*> pl;
    for (auto& x : pool) l.push_back(x), sl.push_back(x), pl.push_back(&x);
    double a = median_ns([&] {
      std::uint64_t s = 0;
      for (auto& x : l) s += x.key;
      sink = s;
    }, N);
    double b = median_ns([&] {
      std::uint64_t s = 0;
      for (auto& x : sl) s += x.key;
      sink = s;
    }, N);
    double c = median_ns([&] {
      std::uint64_t s = 0;
      for (auto* x : pl) s += x->key;
      sink = s;
    }, N);
    std::printf("%-28s %8.2f ns %10.2f ns %10.2f ns\n", "iterate (memory order)", a, b, c);
    l.clear();
  }
  {  // iteration in shuffled memory order
    std::vector<item*> order;
    for (auto& x : pool) order.push_back(&x);
    std::shuffle(order.begin(), order.end(), rng);
    ilist::list<item> l;
    std::list<item> sl;
    std::list<item*> pl;
    for (auto* p : order) l.push_back(*p), sl.push_back(*p), pl.push_back(p);
    double a = median_ns([&] {
      std::uint64_t s = 0;
      for (auto& x : l) s += x.key;
      sink = s;
    }, N);
    double b = median_ns([&] {
      std::uint64_t s = 0;
      for (auto& x : sl) s += x.key;
      sink = s;
    }, N);
    double c = median_ns([&] {
      std::uint64_t s = 0;
      for (auto* x : pl) s += x->key;
      sink = s;
    }, N);
    std::printf("%-28s %8.2f ns %10.2f ns %10.2f ns\n", "iterate (shuffled nodes)", a, b, c);

    // sort by key (7 runs each re-sorting a shuffled order)
    auto reshuffle_i = [&] {
      l.clear();
      for (auto* p : order) l.push_back(*p);
    };
    double sa = 0, sb = 0, sc = 0;
    {
      std::vector<double> r;
      for (int i = 0; i < 7; ++i) {
        reshuffle_i();
        auto t0 = std::chrono::steady_clock::now();
        l.sort([](const item& x, const item& y) { return x.key < y.key; });
        r.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N);
      }
      std::sort(r.begin(), r.end());
      sa = r[3];
    }
    {
      std::vector<double> r;
      for (int i = 0; i < 7; ++i) {
        sl.clear();
        for (auto* p : order) sl.push_back(*p);
        auto t0 = std::chrono::steady_clock::now();
        sl.sort([](const item& x, const item& y) { return x.key < y.key; });
        r.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N);
      }
      std::sort(r.begin(), r.end());
      sb = r[3];
    }
    {
      std::vector<double> r;
      for (int i = 0; i < 7; ++i) {
        pl.assign(order.begin(), order.end());
        auto t0 = std::chrono::steady_clock::now();
        pl.sort([](const item* x, const item* y) { return x->key < y->key; });
        r.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N);
      }
      std::sort(r.begin(), r.end());
      sc = r[3];
    }
    std::printf("%-28s %8.2f ns %10.2f ns %10.2f ns   (per element)\n", "sort 100k by key", sa, sb, sc);
    for (auto it = l.begin(); std::next(it) != l.end(); ++it)
      if (std::next(it)->key < it->key) return 1;
    l.clear();
  }
  return 0;
}
