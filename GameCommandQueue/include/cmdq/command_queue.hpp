// cmdq — очередь команд "любой поток -> главный поток игры".
//
// Идея:
//   * Интерфейс (UI-поток, оверлей, сетевой поток и т.п.) НЕ трогает игровые
//     данные напрямую. Вместо этого он упаковывает действие в команду и кладёт
//     её в очередь: queue.Post([] { player.hp += 10; });
//   * Главный поток игры раз за кадр (в своём цикле, в хуке Present/Tick или
//     в обработчике оконного сообщения) вызывает queue.ExecutePending(),
//     и все команды исполняются там, где данные меняются — без гонок.
//
// Без блокировок:
//   * Производители кладут команду одной CAS-операцией в lock-free стек
//     (Treiber stack). Мьютексов нет, главный поток никогда не ждёт UI.
//   * Главный поток забирает ВСЮ накопленную пачку одной операцией
//     exchange(nullptr), разворачивает её в FIFO-порядок и исполняет.
//     Так как потребитель забирает список целиком, а не по одному узлу,
//     проблемы ABA не возникает.
//   * Команды, добавленные во время исполнения пачки (в том числе самими
//     командами), уходят на следующий вызов — кадр не может "зависнуть"
//     в бесконечном цикле обработки.
//   * Опциональный бюджет времени: ExecutePending(1ms) исполняет команды,
//     пока не выйдет бюджет, остаток переносится на следующий кадр
//     с сохранением порядка.
//
// Гарантии порядка: команды одного потока-производителя исполняются в том
// порядке, в котором были отправлены. Между разными производителями порядок
// определяется моментом успешной вставки.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <thread>
#include <type_traits>
#include <utility>

namespace cmdq {

class CommandQueue {
 public:
  using Clock = std::chrono::steady_clock;
  using ErrorHandler = std::function<void(std::exception_ptr)>;
  using WakeHandler = std::function<void()>;

  CommandQueue() = default;
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

  // Неисполненные команды уничтожаются без вызова. Для команд из Submit()
  // ожидающая сторона получит std::future_error(broken_promise).
  // К моменту разрушения производители не должны обращаться к очереди.
  ~CommandQueue() {
    DestroyList(head_.exchange(nullptr, std::memory_order_acquire));
    DestroyList(backlog_head_);
  }

  // --- Настройка (вызывать до запуска потоков-производителей) ---------------

  // Запоминает текущий поток как главный. Используется Dispatch() и
  // IsOwnerThread(). Вызовите один раз из главного потока игры.
  void BindToCurrentThread() noexcept {
    owner_.store(std::this_thread::get_id(), std::memory_order_release);
  }

  // Вызывается в главном потоке, если команда бросила исключение.
  // Исключение никогда не выходит наружу из ExecutePending(): код игры,
  // в который мы встроены, не должен падать из-за ошибки в команде.
  void SetErrorHandler(ErrorHandler handler) { on_error_ = std::move(handler); }

  // Вызывается в потоке-производителе, когда очередь переходит из пустого
  // состояния в непустое (а не на каждую команду). Позволяет разбудить
  // главный поток, например через PostMessage(hwnd, WM_APP_RUN_COMMANDS).
  // Обработчик должен быть быстрым и потокобезопасным.
  void SetWakeHandler(WakeHandler handler) { on_wake_ = std::move(handler); }

  // --- Производители (любой поток) ------------------------------------------

  // Ставит команду в очередь. Не блокирует. Возвращает false, если очередь
  // закрыта через Close() — команда в этом случае не исполняется.
  template <class F>
  bool Post(F&& fn) {
    if (closed_.load(std::memory_order_acquire)) return false;
    Push(new CallableNode<std::decay_t<F>>(std::forward<F>(fn)));
    return true;
  }

  // Ставит команду в очередь и возвращает future с её результатом. Удобно,
  // когда интерфейсу нужно прочитать игровое состояние:
  //   auto hp = queue.Submit([] { return player.hp; });
  //   ui.Show(hp.get());   // ждёт ТОЛЬКО UI-поток
  // ВАЖНО: не вызывайте future.get() из главного потока — команда исполнится
  // именно там, и получится взаимная блокировка.
  template <class F>
  auto Submit(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>&>> {
    using R = std::invoke_result_t<std::decay_t<F>&>;
    std::promise<R> promise;
    auto future = promise.get_future();
    // Если очередь закрыта, лямбда (а с ней и promise) просто уничтожится,
    // и future вернёт broken_promise.
    Post([promise = std::move(promise), fn = std::forward<F>(fn)]() mutable {
      try {
        if constexpr (std::is_void_v<R>) {
          fn();
          promise.set_value();
        } else {
          promise.set_value(fn());
        }
      } catch (...) {
        promise.set_exception(std::current_exception());
      }
    });
    return future;
  }

  // Если вызвано из главного потока — исполняет команду сразу, иначе ставит
  // в очередь. Возвращает false, только если команда была отклонена Close().
  template <class F>
  bool Dispatch(F&& fn) {
    if (IsOwnerThread()) {
      RunGuarded(fn);
      return true;
    }
    return Post(std::forward<F>(fn));
  }

  // Запрещает новые команды. Уже поставленные будут исполнены при следующих
  // ExecutePending() (или уничтожены деструктором).
  void Close() noexcept { closed_.store(true, std::memory_order_release); }
  bool IsClosed() const noexcept { return closed_.load(std::memory_order_acquire); }

  bool IsOwnerThread() const noexcept {
    return owner_.load(std::memory_order_acquire) == std::this_thread::get_id();
  }

  // --- Главный поток --------------------------------------------------------

  // Исполняет все команды, поставленные до момента вызова.
  // Возвращает количество исполненных команд.
  std::size_t ExecutePending() { return Execute(false, Clock::time_point{}); }

  // То же, но не дольше указанного бюджета (минимум одна команда за вызов,
  // чтобы очередь гарантированно продвигалась). Остаток будет исполнен
  // при следующем вызове в исходном порядке.
  template <class Rep, class Period>
  std::size_t ExecutePending(std::chrono::duration<Rep, Period> budget) {
    return Execute(true, Clock::now() + std::chrono::duration_cast<Clock::duration>(budget));
  }

  // Есть ли что исполнять. Вызывать только из главного потока.
  bool HasPending() const noexcept {
    return backlog_head_ != nullptr || head_.load(std::memory_order_acquire) != nullptr;
  }

 private:
  struct Node {
    Node* next = nullptr;
    virtual void Run() = 0;
    virtual ~Node() = default;
  };

  // Узел и замыкание лежат в одной аллокации; аллокация происходит
  // в потоке-производителе, а не в главном потоке.
  template <class F>
  struct CallableNode final : Node {
    template <class U>
    explicit CallableNode(U&& f) : fn(std::forward<U>(f)) {}
    void Run() override { fn(); }
    F fn;
  };

  void Push(Node* node) noexcept {
    Node* old_head = head_.load(std::memory_order_relaxed);
    do {
      // node ещё не опубликован, поэтому обычная запись безопасна:
      // release в успешном CAS сделает её видимой потребителю.
      node->next = old_head;
    } while (!head_.compare_exchange_weak(old_head, node, std::memory_order_release,
                                          std::memory_order_relaxed));
    if (old_head == nullptr && on_wake_) on_wake_();
  }

  // Забирает всё, что накопили производители, и дописывает в конец backlog
  // в порядке FIFO. backlog принадлежит только главному потоку.
  void Drain() noexcept {
    Node* batch = head_.exchange(nullptr, std::memory_order_acquire);
    if (batch == nullptr) return;

    Node* new_tail = batch;  // самый свежий узел стека станет последним
    Node* new_head = nullptr;
    while (batch != nullptr) {
      Node* next = batch->next;
      batch->next = new_head;
      new_head = batch;
      batch = next;
    }

    if (backlog_tail_ != nullptr) {
      backlog_tail_->next = new_head;
    } else {
      backlog_head_ = new_head;
    }
    backlog_tail_ = new_tail;
  }

  std::size_t Execute(bool has_deadline, Clock::time_point deadline) {
    Drain();
    std::size_t executed = 0;
    while (backlog_head_ != nullptr) {
      // Отцепляем узел до запуска: команда может повторно вызвать
      // ExecutePending() или Dispatch(), и список должен быть согласован.
      Node* node = backlog_head_;
      backlog_head_ = node->next;
      if (backlog_head_ == nullptr) backlog_tail_ = nullptr;

      RunGuarded(*node);
      delete node;
      ++executed;

      if (has_deadline && Clock::now() >= deadline) break;
    }
    return executed;
  }

  template <class F>
  void RunGuarded(F& fn) {
    try {
      if constexpr (std::is_base_of_v<Node, F>) {
        fn.Run();
      } else {
        fn();
      }
    } catch (...) {
      if (on_error_) {
        try {
          on_error_(std::current_exception());
        } catch (...) {
          // Обработчик ошибок тоже не имеет права уронить игру.
        }
      }
    }
  }

  static void DestroyList(Node* node) noexcept {
    while (node != nullptr) {
      Node* next = node->next;
      delete node;
      node = next;
    }
  }

  // std::hardware_destructive_interference_size не используем намеренно:
  // его значение может различаться между единицами трансляции (ABI-предупреждение
  // GCC), а 64 байта верны для всех актуальных x86/ARM.
  static constexpr std::size_t kCacheLine = 64;

  // Общая для всех потоков вершина стека — на отдельной кэш-линии, чтобы
  // запись производителей не инвалидировала данные главного потока.
  alignas(kCacheLine) std::atomic<Node*> head_{nullptr};
  std::atomic<bool> closed_{false};
  std::atomic<std::thread::id> owner_{};

  // Данные только главного потока.
  alignas(kCacheLine) Node* backlog_head_ = nullptr;
  Node* backlog_tail_ = nullptr;
  ErrorHandler on_error_;
  WakeHandler on_wake_;
};

}  // namespace cmdq
