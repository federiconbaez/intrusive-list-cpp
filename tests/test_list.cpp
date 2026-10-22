// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <mutex>
#include <new>
#include <random>
#include <ranges>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "ilist/list.hpp"

// ---- allocation counter: every operator new in this binary goes through here.
// clang's TSan runtime defines operator new itself (duplicate symbols), so the
// counter is off in TSan builds; the ASan/UBSan and plain builds check it.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ILIST_NO_ALLOC_COUNT 1
#endif
#endif
static std::atomic<long> g_allocs{0};
#ifndef ILIST_NO_ALLOC_COUNT
constexpr bool kCountsAllocs = true;
// GCC flags free() on memory from the replaced operator new as a mismatch;
// here both sides are the replacements below, which use malloc/free.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#else
constexpr bool kCountsAllocs = false;
#endif

static int failures = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

struct run_tag {};
struct wait_tag {};

struct node : ilist::hook<>, ilist::hook<run_tag> {
  int key = 0;
  int seq = 0;
  explicit node(int k = 0, int s = 0) : key(k), seq(s) {}
};
using nlist = ilist::list<node>;
using runlist = ilist::list<node, run_tag>;

struct anode : ilist::hook<ilist::default_tag, ilist::link_mode::auto_unlink> {
  int v;
  explicit anode(int x) : v(x) {}
};
using alist = ilist::list<anode, ilist::default_tag, ilist::link_mode::auto_unlink>;

static_assert(std::bidirectional_iterator<nlist::iterator>);
static_assert(std::bidirectional_iterator<nlist::const_iterator>);
static_assert(std::ranges::bidirectional_range<nlist>);
static_assert(std::ranges::common_range<nlist>);
// The safe list carries a size; the hook is two pointers per tag.
static_assert(sizeof(ilist::hook<>) == 2 * sizeof(void*));

template <class L>
static std::vector<int> keys(L& l) {
  std::vector<int> v;
  for (auto& n : l) v.push_back(n.key);
  return v;
}

static void test_basic() {
  node a(1), b(2), c(3);
  nlist l;
  CHECK(l.empty() && l.size() == 0 && l.pop_front() == nullptr && l.check());
  l.push_back(b);
  l.push_front(a);
  l.push_back(c);
  CHECK((keys(l) == std::vector<int>{1, 2, 3}) && l.size() == 3 && l.check());
  CHECK(a.ilist::hook<>::is_linked() && !a.ilist::hook<run_tag>::is_linked());
  std::vector<int> rev;
  for (auto it = l.rbegin(); it != l.rend(); ++it) rev.push_back(it->key);
  CHECK((rev == std::vector<int>{3, 2, 1}));
  auto it = nlist::iterator_to(b);
  it = l.erase(it);
  CHECK(&*it == &c && l.size() == 2 && !b.ilist::hook<>::is_linked());
  l.insert(it, b);
  CHECK((keys(l) == std::vector<int>{1, 2, 3}));
  l.remove(b);
  CHECK(l.pop_back() == &c && l.pop_front() == &a && l.empty() && l.check());
  // ranges algorithms work on it
  l.push_back(a);
  l.push_back(b);
  l.push_back(c);
  auto f = std::ranges::find_if(l, [](const node& n) { return n.key == 2; });
  CHECK(&*f == &b);
  CHECK(std::ranges::distance(l | std::views::reverse) == 3);
  l.clear();
  CHECK(!a.ilist::hook<>::is_linked() && !c.ilist::hook<>::is_linked());
}

static void test_two_lists_at_once() {
  node n[4] = {node(0), node(1), node(2), node(3)};
  nlist all;
  runlist running;
  for (auto& x : n) all.push_back(x);
  running.push_back(n[3]);
  running.push_back(n[1]);
  CHECK((keys(all) == std::vector<int>{0, 1, 2, 3}));
  CHECK((keys(running) == std::vector<int>{3, 1}));
  all.remove(n[1]);
  CHECK(running.size() == 2 && running.check() && all.check());
  running.clear();
  all.clear();
}

static void test_splice() {
  node n[8];
  for (int i = 0; i < 8; ++i) n[i].key = i;
  nlist a, b;
  for (int i = 0; i < 4; ++i) a.push_back(n[i]);
  for (int i = 4; i < 8; ++i) b.push_back(n[i]);

  a.splice(nlist::iterator_to(n[2]), b);  // whole list into the middle
  CHECK((keys(a) == std::vector<int>{0, 1, 4, 5, 6, 7, 2, 3}) && b.empty() && a.size() == 8 && a.check() && b.check());

  b.splice(b.end(), a, nlist::iterator_to(n[5]));  // one element across
  CHECK((keys(b) == std::vector<int>{5}) && a.size() == 7 && b.size() == 1 && a.check() && b.check());

  // range across lists: [4, 2) = 4 6 7
  b.splice(b.begin(), a, nlist::iterator_to(n[4]), nlist::iterator_to(n[2]));
  CHECK((keys(b) == std::vector<int>{4, 6, 7, 5}) && (keys(a) == std::vector<int>{0, 1, 2, 3}));
  CHECK(a.size() == 4 && b.size() == 4 && a.check() && b.check());

  // range within one list: move [0, 2) to the end
  a.splice(a.end(), a, a.begin(), nlist::iterator_to(n[2]));
  CHECK((keys(a) == std::vector<int>{2, 3, 0, 1}) && a.size() == 4 && a.check());
  // single element onto its own position: no-op
  a.splice(nlist::iterator_to(n[3]), a, nlist::iterator_to(n[3]));
  a.splice(nlist::iterator_to(n[0]), a, nlist::iterator_to(n[3]));
  CHECK((keys(a) == std::vector<int>{2, 3, 0, 1}) && a.check());
  // self splice of whole list: no-op
  a.splice(a.begin(), a);
  CHECK(a.size() == 4 && a.check());

  // move construction and assignment carry the elements
  nlist c(std::move(a));
  CHECK(a.empty() && c.size() == 4 && c.check());
  b = std::move(c);
  CHECK(c.empty() && b.size() == 4 && (keys(b) == std::vector<int>{2, 3, 0, 1}));
  b.clear();
}

static void test_sort_stable() {
  std::mt19937 rng(7);
  for (int n : {0, 1, 2, 3, 7, 64, 1000, 4097}) {
    std::vector<node> v(static_cast<std::size_t>(n));
    nlist l;
    for (int i = 0; i < n; ++i) {
      v[static_cast<std::size_t>(i)] = node(static_cast<int>(rng() % 17), i);
      l.push_back(v[static_cast<std::size_t>(i)]);
    }
    const long before = g_allocs.load();
    l.sort([](const node& a, const node& b) { return a.key < b.key; });
    CHECK(g_allocs.load() == before);
    CHECK(l.check() && l.size() == static_cast<std::size_t>(n));
    std::vector<std::pair<int, int>> got, want;
    for (auto& x : l) got.emplace_back(x.key, x.seq);
    for (auto& x : v) want.emplace_back(x.key, x.seq);
    std::stable_sort(want.begin(), want.end(), [](auto& a, auto& b) { return a.first < b.first; });
    CHECK(got == want);
    // prev links are rebuilt: walking backwards gives the reverse
    std::vector<std::pair<int, int>> back;
    for (auto it = l.rbegin(); it != l.rend(); ++it) back.emplace_back(it->key, it->seq);
    std::reverse(back.begin(), back.end());
    CHECK(back == got);
    l.clear();
  }
}

// Random operations against std::list<node*>, checking links after each.
static void test_model() {
  std::mt19937 rng(2026);
  constexpr int N = 64;
  std::vector<node> pool(N);
  for (int i = 0; i < N; ++i) pool[static_cast<std::size_t>(i)].key = i;
  nlist a, b;
  std::list<node*> ma, mb;
  auto in_any = [&](node& n) { return n.ilist::hook<>::is_linked(); };
  for (int step = 0; step < 200000; ++step) {
    node& n = pool[rng() % N];
    const bool use_a = rng() & 1;
    nlist& l = use_a ? a : b;
    std::list<node*>& m = use_a ? ma : mb;
    switch (rng() % 7) {
      case 0:
        if (!in_any(n)) l.push_back(n), m.push_back(&n);
        break;
      case 1:
        if (!in_any(n)) l.push_front(n), m.push_front(&n);
        break;
      case 2:
        if (auto* p = l.pop_front()) {
          CHECK(p == m.front());
          m.pop_front();
        }
        break;
      case 3: {
        auto mit = std::find(m.begin(), m.end(), &n);
        if (mit != m.end()) l.remove(n), m.erase(mit);
        break;
      }
      case 4: {  // move n (if in the other list) before a random element of l
        nlist& o = use_a ? b : a;
        std::list<node*>& mo = use_a ? mb : ma;
        auto mit = std::find(mo.begin(), mo.end(), &n);
        if (mit == mo.end()) break;
        auto pos = l.begin();
        auto mpos = m.begin();
        for (std::size_t k = rng() % (m.size() + 1); k > 0; --k) ++pos, ++mpos;
        l.splice(pos, o, nlist::iterator_to(n));
        m.splice(mpos, mo, mit);
        break;
      }
      case 5:
        if (rng() % 50 == 0) l.splice(l.end(), use_a ? b : a), m.splice(m.end(), use_a ? mb : ma);
        break;
      case 6:
        if (rng() % 200 == 0) {
          l.sort([](const node& x, const node& y) { return x.key < y.key; });
          m.sort([](const node* x, const node* y) { return x->key < y->key; });
        }
        break;
    }
    if (step % 97 == 0) {
      CHECK(a.check() && b.check());
      CHECK(a.size() == ma.size() && b.size() == mb.size());
      CHECK(std::equal(a.begin(), a.end(), ma.begin(), ma.end(), [](node& x, node* y) { return &x == y; }));
      CHECK(std::equal(b.begin(), b.end(), mb.begin(), mb.end(), [](node& x, node* y) { return &x == y; }));
      if (failures) return;
    }
  }
  a.clear();
  b.clear();
}

static void test_no_allocations() {
  if (!kCountsAllocs) {
    std::puts("allocation counting off in this build (TSan)");
    return;
  }
  std::vector<node> v(1000);
  nlist a, b;
  const long before = g_allocs.load();
  for (auto& x : v) a.push_back(x);
  for (int r = 0; r < 100; ++r) {
    b.splice(b.end(), a);
    while (node* p = b.pop_front()) a.push_back(*p);
    a.splice(a.begin(), a, std::next(a.begin(), 10), std::next(a.begin(), 500));
  }
  a.sort([](const node& x, const node& y) { return &x < &y; });
  CHECK(g_allocs.load() == before);
  a.clear();
}

// These two tests destroy elements while linked on purpose (auto-unlink and
// the abort in safe mode); GCC's -Wdangling-pointer cannot see that the hook
// destructor unlinks or aborts first.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
static void test_auto_unlink_scope_exit() {
  alist l;
  anode keep1(1), keep2(4);
  l.push_back(keep1);
  {
    anode t2(2), t3(3);
    l.push_back(t2);
    l.push_back(t3);
    CHECK(l.size() == 3);
  }  // t2, t3 unlink themselves here
  l.push_back(keep2);
  std::vector<int> got;
  for (auto& x : l) got.push_back(x.v);
  CHECK((got == std::vector<int>{1, 4}) && l.check());
  keep1.unlink();
  CHECK(l.size() == 1 && l.check());
  {
    auto* heap = new anode(9);
    l.push_front(*heap);
    delete heap;  // no dangling node left behind
  }
  CHECK(l.size() == 1 && &l.front() == &keep2 && l.check());
  // Copying an element does not copy its membership.
  anode copy = keep2;
  CHECK(!copy.is_linked() && keep2.is_linked());
}

static void test_safe_mode_aborts_on_linked_destruction() {
  std::fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    // Silence the expected message in the child.
    std::freopen("/dev/null", "w", stderr);
    nlist l;
    {
      node n(1);
      l.push_back(n);
    }
    _exit(0);  // not reached
  }
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Nodes handed between threads through two lists under one mutex: the
// list itself has no hidden shared state, so TSan must stay quiet and
// every node must be accounted for at the end.
static void test_threads_handoff() {
  constexpr int kNodes = 256, kThreads = 4, kIters = 20000;
  std::vector<node> v(kNodes);
  nlist free_list, busy;
  std::mutex mu;
  for (auto& x : v) free_list.push_back(x);
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t));
      for (int i = 0; i < kIters; ++i) {
        std::lock_guard g(mu);
        nlist& from = (rng() & 1) ? free_list : busy;
        nlist& to = (&from == &free_list) ? busy : free_list;
        if (node* p = from.pop_front()) {
          p->seq++;
          to.push_back(*p);
        }
      }
    });
  }
  for (auto& th : ts) th.join();
  CHECK(free_list.size() + busy.size() == kNodes && free_list.check() && busy.check());
  long moves = 0;
  for (auto& x : v) moves += x.seq;
  CHECK(moves > 0 && moves <= static_cast<long>(kThreads) * kIters);
  free_list.clear();
  busy.clear();
}

int main() {
  test_basic();
  test_two_lists_at_once();
  test_splice();
  test_sort_stable();
  test_model();
  test_no_allocations();
  test_auto_unlink_scope_exit();
  test_safe_mode_aborts_on_linked_destruction();
  test_threads_handoff();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("all checks passed");
  return 0;
}
