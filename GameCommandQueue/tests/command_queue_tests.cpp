// Тесты без внешних зависимостей. Запускайте также под -DCMDQ_SANITIZE=thread:
// игровые данные в тестах намеренно НЕ атомарные, поэтому любая гонка
// будет найдена ThreadSanitizer'ом.

#include "cmdq/command_queue.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace std::chrono_literals;

static void TestFifoSingleProducer() {
  cmdq::CommandQueue q;
  std::vector<int> order;
  for (int i = 0; i < 100; ++i) q.Post([&order, i] { order.push_back(i); });
  CHECK(q.ExecutePending() == 100);
  CHECK(order.size() == 100);
  for (int i = 0; i < 100; ++i) CHECK(order[i] == i);
  CHECK(!q.HasPending());
  CHECK(q.ExecutePending() == 0);
}

static void TestMultiProducerOrderAndCount() {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 50000;

  cmdq::CommandQueue q;
  q.BindToCurrentThread();

  // "Игровое состояние": меняется только в главном потоке, без атомиков.
  int last_seen[kProducers];
  for (int& v : last_seen) v = -1;
  long long total = 0;
  bool order_ok = true;

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        q.Post([&, p, i] {
          if (last_seen[p] + 1 != i) order_ok = false;
          last_seen[p] = i;
          ++total;
        });
      }
    });
  }

  // Главный цикл: крутим кадры, пока всё не исполнится.
  const long long expected = static_cast<long long>(kProducers) * kPerProducer;
  while (total < expected) {
    q.ExecutePending();
    std::this_thread::yield();
  }
  for (auto& t : producers) t.join();
  q.ExecutePending();

  CHECK(order_ok);
  CHECK(total == expected);
  for (int v : last_seen) CHECK(v == kPerProducer - 1);
}

static void TestCommandsPostedDuringExecutionRunNextFrame() {
  cmdq::CommandQueue q;
  int runs = 0;
  // Команда, которая перевыставляет сама себя: без защиты кадр бы завис.
  std::function<void()> self;
  self = [&] {
    ++runs;
    q.Post(self);
  };
  q.Post(self);
  CHECK(q.ExecutePending() == 1);
  CHECK(runs == 1);
  CHECK(q.ExecutePending() == 1);
  CHECK(runs == 2);
}

static void TestBudgetCarriesOverInOrder() {
  cmdq::CommandQueue q;
  std::vector<int> order;
  for (int i = 0; i < 10; ++i) {
    q.Post([&order, i] {
      order.push_back(i);
      std::this_thread::sleep_for(2ms);
    });
  }
  std::size_t first = q.ExecutePending(3ms);
  CHECK(first >= 1);
  CHECK(first < 10);

  // Новая команда должна встать ПОСЛЕ перенесённого остатка.
  q.Post([&order] { order.push_back(100); });

  // Нулевой бюджет — всё равно ровно одна команда за вызов (прогресс).
  CHECK(q.ExecutePending(0ms) == 1);

  while (q.HasPending()) q.ExecutePending();
  CHECK(order.size() == 11);
  for (int i = 0; i < 10; ++i) CHECK(order[i] == i);
  CHECK(order[10] == 100);
}

static void TestExceptionsAreContained() {
  cmdq::CommandQueue q;
  std::string error;
  q.SetErrorHandler([&](std::exception_ptr e) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      error = ex.what();
    }
  });
  bool after = false;
  q.Post([] { throw std::runtime_error("boom"); });
  q.Post([&] { after = true; });
  CHECK(q.ExecutePending() == 2);
  CHECK(error == "boom");
  CHECK(after);

  // Без обработчика исключение тоже не выходит наружу.
  cmdq::CommandQueue silent;
  silent.Post([] { throw 42; });
  CHECK(silent.ExecutePending() == 1);
}

static void TestSubmitReturnsValueFromMainThread() {
  cmdq::CommandQueue q;
  q.BindToCurrentThread();
  const auto main_id = std::this_thread::get_id();
  int gold = 500;  // данные главного потока

  std::atomic<bool> done{false};
  int result = 0;
  bool ran_on_main = false;
  bool got_exception = false;

  std::thread ui([&] {
    auto fut = q.Submit([&] {
      ran_on_main = std::this_thread::get_id() == main_id;
      return gold;
    });
    result = fut.get();

    auto bad = q.Submit([]() -> int { throw std::logic_error("no"); });
    try {
      bad.get();
    } catch (const std::logic_error&) {
      got_exception = true;
    }

    auto void_fut = q.Submit([&] { gold += 1; });
    void_fut.get();
    done = true;
  });

  while (!done) {
    q.ExecutePending();
    std::this_thread::yield();
  }
  ui.join();
  CHECK(result == 500);
  CHECK(ran_on_main);
  CHECK(got_exception);
  CHECK(gold == 501);
}

static void TestCloseRejectsAndBreaksPromise() {
  cmdq::CommandQueue q;
  q.Close();
  CHECK(q.IsClosed());
  bool ran = false;
  CHECK(!q.Post([&] { ran = true; }));
  auto fut = q.Submit([] { return 1; });
  bool broken = false;
  try {
    fut.get();
  } catch (const std::future_error& e) {
    broken = e.code() == std::future_errc::broken_promise;
  }
  CHECK(broken);
  CHECK(q.ExecutePending() == 0);
  CHECK(!ran);
}

static void TestDestructorFreesPendingCommands() {
  static int alive = 0;
  struct Tracker {
    Tracker() { ++alive; }
    Tracker(const Tracker&) { ++alive; }
    Tracker(Tracker&&) noexcept { ++alive; }
    ~Tracker() { --alive; }
  };
  std::future<int> fut;
  {
    cmdq::CommandQueue q;
    // Нулевой бюджет исполнит только первую команду, остальные пять
    // останутся в backlog; ещё две команды останутся в lock-free стеке.
    q.Post([] {});
    for (int i = 0; i < 5; ++i) q.Post([t = Tracker{}] {});
    CHECK(q.ExecutePending(0ms) == 1);
    q.Post([t = Tracker{}] {});
    fut = q.Submit([] { return 7; });
    CHECK(alive == 6);
  }
  CHECK(alive == 0);
  bool broken = false;
  try {
    fut.get();
  } catch (const std::future_error&) {
    broken = true;
  }
  CHECK(broken);
}

static void TestDispatchRunsInlineOnOwner() {
  cmdq::CommandQueue q;
  q.BindToCurrentThread();
  int value = 0;
  CHECK(q.Dispatch([&] { value = 1; }));
  CHECK(value == 1);  // исполнено сразу, без очереди
  CHECK(!q.HasPending());

  std::thread other([&] { q.Dispatch([&] { value = 2; }); });
  other.join();
  CHECK(value == 1);
  CHECK(q.ExecutePending() == 1);
  CHECK(value == 2);
}

static void TestWakeOnlyOnEmptyToNonEmpty() {
  cmdq::CommandQueue q;
  std::atomic<int> wakes{0};
  q.SetWakeHandler([&] { ++wakes; });
  q.Post([] {});
  q.Post([] {});
  q.Post([] {});
  CHECK(wakes == 1);
  q.ExecutePending();
  q.Post([] {});
  CHECK(wakes == 2);
  q.ExecutePending();
}

int main() {
  TestFifoSingleProducer();
  TestMultiProducerOrderAndCount();
  TestCommandsPostedDuringExecutionRunNextFrame();
  TestBudgetCarriesOverInOrder();
  TestExceptionsAreContained();
  TestSubmitReturnsValueFromMainThread();
  TestCloseRejectsAndBreaksPromise();
  TestDestructorFreesPendingCommands();
  TestDispatchRunsInlineOnOwner();
  TestWakeOnlyOnEmptyToNonEmpty();
  std::puts("All command queue tests passed");
  return 0;
}
