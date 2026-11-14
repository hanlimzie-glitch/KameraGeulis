#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>
#include "kgcore/bounded_queue.h"

using kgcore::BoundedQueue;

TEST_CASE("bounded queue keeps FIFO order") {
  BoundedQueue<int> q(4);
  q.Push(1); q.Push(2); q.Push(3);
  int v = 0;
  REQUIRE(q.TryPop(v)); REQUIRE(v == 1);
  REQUIRE(q.TryPop(v)); REQUIRE(v == 2);
  REQUIRE(q.TryPop(v)); REQUIRE(v == 3);
  REQUIRE_FALSE(q.TryPop(v));
}

TEST_CASE("bounded queue drops oldest when full") {
  BoundedQueue<int> q(2);
  REQUIRE_FALSE(q.Push(1));
  REQUIRE_FALSE(q.Push(2));
  REQUIRE(q.Push(3));  // evicts 1
  REQUIRE(q.Size() == 2);
  int v = 0;
  REQUIRE(q.TryPop(v)); REQUIRE(v == 2);
  REQUIRE(q.TryPop(v)); REQUIRE(v == 3);
}

TEST_CASE("bounded queue capacity 1 always holds the newest item") {
  BoundedQueue<std::string> q(1);
  q.Push("old");
  q.Push("new");
  std::string s;
  REQUIRE(q.TryPop(s));
  REQUIRE(s == "new");
}

TEST_CASE("bounded queue survives interleaved push/pop") {
  BoundedQueue<int> q(3);
  int v = 0;
  q.Push(1); q.Push(2);
  q.TryPop(v);  // 1
  q.Push(3); q.Push(4); q.Push(5);  // 2,3,4 then push 5 drops 2
  std::vector<int> got;
  while (q.TryPop(v)) got.push_back(v);
  REQUIRE(got == std::vector<int>{3, 4, 5});
}

TEST_CASE("clear empties the queue") {
  BoundedQueue<int> q(2);
  q.Push(1); q.Push(2); q.Clear();
  REQUIRE(q.Empty());
  REQUIRE(q.Size() == 0);
}
