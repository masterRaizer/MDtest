// Демо: "игра" крутит главный цикл ~60 FPS и меняет мир каждый кадр,
// а отдельный UI-поток (оверлей/меню) управляет миром только через очередь.

#include "cmdq/command_queue.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;

// Данные игры. Принадлежат главному потоку, синхронизации нет совсем.
struct GameWorld {
  int frame = 0;
  int player_hp = 100;
  int gold = 0;
  bool god_mode = false;
};

int main() {
  GameWorld world;
  cmdq::CommandQueue commands;
  commands.BindToCurrentThread();
  commands.SetErrorHandler([](std::exception_ptr) { std::puts("[game] command failed"); });

  std::atomic<bool> running{true};

  // Поток интерфейса: ничего не знает о синхронизации игры.
  std::thread ui([&] {
    commands.Post([&] { world.gold += 250; });
    commands.Post([&] { world.god_mode = true; });

    std::this_thread::sleep_for(50ms);
    commands.Post([&] { world.player_hp = 100; });

    // Чтение состояния: ждёт только UI-поток, главный цикл не блокируется.
    auto snapshot = commands.Submit([&] { return world; }).get();
    std::printf("[ui]   frame=%d hp=%d gold=%d god=%d\n", snapshot.frame, snapshot.player_hp,
                snapshot.gold, snapshot.god_mode);

    commands.Post([] { throw std::runtime_error("bad command"); });
    commands.Post([&] { running = false; });
  });

  // Главный цикл игры (в реальном моде это хук Present/Tick игры).
  while (running) {
    const auto frame_start = std::chrono::steady_clock::now();

    // 1) Команды из интерфейса — не больше 1 мс на кадр.
    commands.ExecutePending(1ms);

    // 2) Обычная логика кадра, свободно меняющая те же данные.
    ++world.frame;
    if (!world.god_mode && world.player_hp > 0) --world.player_hp;

    std::this_thread::sleep_until(frame_start + 16ms);
  }
  commands.Close();
  ui.join();
  commands.ExecutePending();

  std::printf("[game] finished at frame=%d hp=%d gold=%d\n", world.frame, world.player_hp,
              world.gold);
  return 0;
}
