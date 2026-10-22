// SPDX-License-Identifier: Apache-2.0
// Intrusive doubly linked list. The links live inside the element (a base
// class hook), so push, pop, erase and splice never allocate. An element can
// be in several lists at once through different tags.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <utility>

namespace ilist {

enum class link_mode {
  /// Destroying a linked element aborts: it would leave a dangling node.
  safe,
  /// Destroying a linked element unlinks it. The list then cannot keep an
  /// O(1) size, so size() walks the list.
  auto_unlink,
};

struct default_tag {};

namespace detail {
struct links {
  links* prev = nullptr;
  links* next = nullptr;
};
}  // namespace detail

template <class T, class Tag, link_mode M>
class list;

/// Base class hook: `struct job : ilist::hook<> { ... };`
template <class Tag = default_tag, link_mode M = link_mode::safe>
class hook {
 public:
  hook() noexcept = default;
  // Copying or moving an element never copies its membership.
  hook(const hook&) noexcept {}
  hook& operator=(const hook&) noexcept { return *this; }
  ~hook() {
    if (!is_linked()) return;
    if constexpr (M == link_mode::auto_unlink) {
      unlink();
    } else {
      std::fputs("ilist: destroying an element that is still linked\n", stderr);
      std::abort();
    }
  }

  [[nodiscard]] bool is_linked() const noexcept { return l_.next != nullptr; }

  /// Removes the element from whatever list holds it. Only valid without
  /// O(1) size tracking, i.e. for auto_unlink hooks.
  void unlink() noexcept
    requires(M == link_mode::auto_unlink)
  {
    if (!is_linked()) return;
    l_.prev->next = l_.next;
    l_.next->prev = l_.prev;
    l_.prev = l_.next = nullptr;
  }

 private:
  template <class, class, link_mode>
  friend class list;
  detail::links l_;
};

/// List of `T`, where `T` derives from `hook<Tag, M>`.
template <class T, class Tag = default_tag, link_mode M = link_mode::safe>
class list {
  using hook_t = hook<Tag, M>;
  using links = detail::links;
  static constexpr bool kCountsSize = (M == link_mode::safe);

  static links* L(T& v) noexcept { return &static_cast<hook_t&>(v).l_; }
  static T* V(links* l) noexcept {
    // links is the only member of hook_t, so a hook and its links share an
    // address; the downcast from hook to T is a plain static_cast.
    return static_cast<T*>(reinterpret_cast<hook_t*>(l));
  }

 public:
  template <bool Const>
  class iter {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using iterator_concept = std::bidirectional_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<Const, const T*, T*>;
    using reference = std::conditional_t<Const, const T&, T&>;

    iter() noexcept = default;
    explicit iter(links* p) noexcept : p_(p) {}
    template <bool C = Const>
      requires C
    iter(const iter<false>& o) noexcept : p_(o.p_) {}

    reference operator*() const noexcept { return *V(p_); }
    pointer operator->() const noexcept { return V(p_); }
    iter& operator++() noexcept {
      p_ = p_->next;
      return *this;
    }
    iter operator++(int) noexcept {
      iter t = *this;
      ++*this;
      return t;
    }
    iter& operator--() noexcept {
      p_ = p_->prev;
      return *this;
    }
    iter operator--(int) noexcept {
      iter t = *this;
      --*this;
      return t;
    }
    friend bool operator==(const iter& a, const iter& b) noexcept { return a.p_ == b.p_; }

   private:
    friend class list;
    template <bool>
    friend class iter;
    links* p_ = nullptr;
  };
  using iterator = iter<false>;
  using const_iterator = iter<true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using value_type = T;
  using size_type = std::size_t;

  list() noexcept { root_.prev = root_.next = &root_; }
  list(const list&) = delete;
  list& operator=(const list&) = delete;
  list(list&& o) noexcept : list() { splice(end(), o); }
  list& operator=(list&& o) noexcept {
    if (this != &o) {
      clear();
      splice(end(), o);
    }
    return *this;
  }
  /// Unlinks every element; the elements themselves are not touched.
  ~list() { clear(); }

  iterator begin() noexcept { return iterator(root_.next); }
  iterator end() noexcept { return iterator(&root_); }
  const_iterator begin() const noexcept { return const_iterator(root_.next); }
  const_iterator end() const noexcept { return const_iterator(const_cast<links*>(&root_)); }
  reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
  reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

  [[nodiscard]] bool empty() const noexcept { return root_.next == &root_; }

  /// O(1) for safe hooks, O(n) for auto_unlink hooks.
  [[nodiscard]] size_type size() const noexcept {
    if constexpr (kCountsSize) {
      return size_;
    } else {
      size_type n = 0;
      for (const links* p = root_.next; p != &root_; p = p->next) ++n;
      return n;
    }
  }

  T& front() noexcept { return *V(root_.next); }
  T& back() noexcept { return *V(root_.prev); }

  void push_front(T& v) noexcept { link_before(root_.next, L(v)); }
  void push_back(T& v) noexcept { link_before(&root_, L(v)); }
  /// Inserts before `pos`; returns an iterator to `v`.
  iterator insert(iterator pos, T& v) noexcept {
    link_before(pos.p_, L(v));
    return iterator(L(v));
  }

  T* pop_front() noexcept { return empty() ? nullptr : &unlink_node(root_.next); }
  T* pop_back() noexcept { return empty() ? nullptr : &unlink_node(root_.prev); }

  /// Removes the element at `pos`; returns the next position.
  iterator erase(iterator pos) noexcept {
    links* next = pos.p_->next;
    unlink_node(pos.p_);
    return iterator(next);
  }
  /// O(1) removal of an element known to be in this list.
  void remove(T& v) noexcept { unlink_node(L(v)); }

  /// The position of an element known to be in this list, in O(1).
  static iterator iterator_to(T& v) noexcept { return iterator(L(v)); }

  void clear() noexcept {
    links* p = root_.next;
    while (p != &root_) {
      links* n = p->next;
      p->prev = p->next = nullptr;
      p = n;
    }
    root_.prev = root_.next = &root_;
    if constexpr (kCountsSize) size_ = 0;
  }

  /// Moves every element of `other` before `pos`. O(1).
  void splice(iterator pos, list& other) noexcept {
    if (&other == this || other.empty()) return;
    if constexpr (kCountsSize) {
      size_ += other.size_;
      other.size_ = 0;
    }
    move_range(pos.p_, other.root_.next, &other.root_);
    other.root_.prev = other.root_.next = &other.root_;
  }

  /// Moves the element at `it` of `other` before `pos`. O(1).
  void splice(iterator pos, list& other, iterator it) noexcept {
    if (pos == it || pos.p_ == it.p_->next) return;
    other.unlink_node(it.p_);
    link_before(pos.p_, it.p_);
  }

  /// Moves [first, last) of `other` before `pos`. O(1) within one list or
  /// for auto_unlink lists; O(k) across safe lists, to keep sizes exact.
  /// `pos` must not lie inside [first, last).
  void splice(iterator pos, list& other, iterator first, iterator last) noexcept {
    if (first == last || pos == last) return;
    if constexpr (kCountsSize) {
      if (&other != this) {
        size_type k = 0;
        for (links* p = first.p_; p != last.p_; p = p->next) ++k;
        other.size_ -= k;
        size_ += k;
      }
    }
    move_range(pos.p_, first.p_, last.p_);
  }

  /// Stable merge sort, O(n log n), no allocation.
  template <class Less = std::less<>>
  void sort(Less less = {}) {
    if (root_.next == root_.prev) return;  // 0 or 1 element
    // Detach into a null-terminated singly linked chain via `next`.
    links* head = root_.next;
    root_.prev->next = nullptr;
    head = merge_sort(head, less);
    // Rebuild prev pointers and close the ring.
    links* prev = &root_;
    root_.next = head;
    for (links* p = head; p; p = p->next) {
      p->prev = prev;
      prev = p;
    }
    prev->next = &root_;
    root_.prev = prev;
  }

  /// Walks the ring and checks every link and the size. For tests.
  [[nodiscard]] bool check() const noexcept {
    size_type n = 0;
    const links* p = &root_;
    do {
      if (!p->next || !p->prev || p->next->prev != p) return false;
      p = p->next;
      if (p != &root_ && ++n > (size_type{1} << 40)) return false;
    } while (p != &root_);
    if constexpr (kCountsSize) return n == size_;
    return true;
  }

 private:
  static void link_between(links* prev, links* next, links* n) noexcept {
    n->prev = prev;
    n->next = next;
    prev->next = n;
    next->prev = n;
  }
  void link_before(links* pos, links* n) noexcept {
    link_between(pos->prev, pos, n);
    if constexpr (kCountsSize) ++size_;
  }
  T& unlink_node(links* n) noexcept {
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = nullptr;
    if constexpr (kCountsSize) --size_;
    return *V(n);
  }
  /// Moves the chain [first, last) (in any list) before pos.
  static void move_range(links* pos, links* first, links* last) noexcept {
    links* tail = last->prev;
    // cut
    first->prev->next = last;
    last->prev = first->prev;
    // paste
    links* before = pos->prev;
    before->next = first;
    first->prev = before;
    tail->next = pos;
    pos->prev = tail;
  }

  template <class Less>
  static links* merge_sort(links* head, Less& less) {
    // Bottom-up merge sort on a singly linked chain: no recursion, no
    // allocation. Equal elements keep their order.
    if (!head || !head->next) return head;
    std::size_t width = 1;
    for (;;) {
      links* p = head;
      links* out = nullptr;
      links** tail = &out;
      std::size_t merges = 0;
      while (p) {
        ++merges;
        links* q = p;
        std::size_t psize = 0;
        for (; psize < width && q; ++psize) q = q->next;
        std::size_t qsize = width;
        while (psize > 0 || (qsize > 0 && q)) {
          links* e;
          if (psize == 0) {
            e = q, q = q->next, --qsize;
          } else if (qsize == 0 || !q) {
            e = p, p = p->next, --psize;
          } else if (less(*V(q), *V(p))) {
            e = q, q = q->next, --qsize;
          } else {
            e = p, p = p->next, --psize;
          }
          *tail = e;
          tail = &e->next;
        }
        p = q;
      }
      *tail = nullptr;
      head = out;
      if (merges <= 1) return head;
      width *= 2;
    }
  }

  links root_;
  [[no_unique_address]] std::conditional_t<kCountsSize, size_type, std::byte> size_{};
};

}  // namespace ilist
