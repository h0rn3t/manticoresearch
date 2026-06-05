# Tasks

> **Окружение проверено.** Локально (colima arm64, ядро 6.8): smoke проходит при
> `seccomp=unconfined`, падает с `EPERM` под дефолтным seccomp, и `ENOSYS` под amd64/Rosetta
> — подтверждает необходимость fallback (задачи 1.2, 6.4).
> **CI Phase 1 ЗЕЛЁНАЯ** (`.github/workflows/iouring_probe.yml`, нативный x86_64 ubuntu-24.04):
> raw smoke + backend-модуль + 8 конкурентных чтений + стаб-ветка — всё OK за 22с.
> ⇒ GitHub Actions = валидный стенд для x86_64-сборки и A/B-бенчей.

## 1. Сборка и детект
- [x] 1.1 Найти `liburing` в CMake, ввести `HAVE_IO_URING`, опцию сборки
      → `cmake/Findliburing.cmake`, `with_menu_libname` (Linux-only) в `CMakeLists.txt`,
        линковка + `HAVE_IO_URING=1` в `src/CMakeLists.txt`
- [x] 1.2 Runtime-детект `io_uring_setup` → `IoUring::IsIoUringAvailable()` (проба + кэш;
      откат при EPERM/ENOSYS). Подтверждено: EPERM под seccomp, ENOSYS под amd64-эмуляцией.
- [x] 1.3 Опция конфига `searchd.io_uring = 1|0` (дефолт — авто/вкл)
      → `g_bIoUring` = `GetBool("io_uring", true)` (`searchd.cpp`); старт после fork —
        `StartIoUring`+`InstallIoUringReadHook`, `StopIoUring` на shutdown. Также
        `io_uring_sqpoll` и `io_uring_max_inflight` (см. 2.5)

## 2. io_uring backend (новый модуль) — `src/iouring.{h,cpp}`, верифицирован запуском
- [x] 2.1 Ring lifecycle: `StartIoUring`/`StopIoUring`, глубина очереди, NOP-sentinel teardown
- [x] 2.2 Submit-API: `SubmitRead` → `prep_read` + `user_data` = указатель на `Completion_t`
      (callback-based; корутинная склейка передаст callback с `Waker.Wake()`)
- [x] 2.3 Выделенный io-поток-reaper: `io_uring_wait_cqe` → разбор `user_data` → callback
- [x] 2.4 Thread-safe submit из воркеров (mutex вокруг SQ) — проверено 8 конкурентными чтениями
- [x] 2.5 Ограничение inflight SQE (замена логики throttling)
      → атомарный счётчик `m_iInflight` + cap `m_uMaxInflight` в `iouring.cpp`: при достижении
        cap `SubmitRead` возвращает false → фолбэк на `sphPread`. Конфиг `io_uring_max_inflight`
        (0 = глубина кольца, сохраняет прежнее поведение). NB: написано на darwin, локально
        не собрано — собрать/прогнать на CI (x86_64 ubuntu)

## 3. Корутинная интеграция
- [x] 3.1 Хелпер `SubmitReadAndYield(fd, buf, len, off) -> bytes` поверх `Waker_c`/`YieldWith`
      → `IoUringPreadCoro` в `src/iouring_coro.cpp` (хук `sphPreadCoro`): submit внутри
        `YieldWith`; вне корутины / при недоступности backend — `sphPread`
- [x] 3.2 Корректная передача результата (число байт/ошибка) обратно в фибру
      → результат в heap-`ReadState_t` (shared_ptr, всё захватывается by value — фибра может
        резюмиться до возврата из YieldWith-хендлера, нельзя ссылаться на стек фибры)
- [x] 3.3 Обработка частичного чтения и ошибок (errno из CQE res)
      → `m_iResult` зеркалит pread (`>=0` байт / `-errno`); `UpdateCache` ловит `<0` → `m_bError`

## 4. FileReader интеграция
- [x] 4.1 `FileReader_c::FillBuffer`: async-путь под корутиной, pread — иначе/при выкл io_uring
      → `CSphReader::UpdateCache` (`fileio.cpp`): `m_bAsyncReads ? sphPreadCoro : sphPread`
- [x] 4.2 Проброс «async-capable» через `DirectFileReader_c`/`DirectFactory_c`
      → ctor `DirectFileReader_c`: `SetAsyncReads(true)` — readers создаются per-query и не
        шарятся между фибрами (docstore-ридеры остаются синхронными)
- [x] 4.3 Учёт IO-статистики (`CSphIOStats`) в async-пути
      → `IoUringPreadCoro`: `GetIOStats()` → `m_iReadOps` / `m_iReadBytes`

## 5. Дроп не-Linux бэкендов (Linux-only)
> NB: §5 написан вслепую на darwin (io_uring/epoll-путь локально не собирается). На Linux
> epoll и так был единственным живым бэкендом, так что удалён только мёртвый на Linux код
> (kqueue/poll/win32) — собрать/прогнать на CI (x86_64 ubuntu).
- [x] 5.1 `netpoll.cpp`: убрать `NETPOLL_KQUEUE`/`NETPOLL_POLL`/Windows, оставить epoll
      → `netpoll.h`: селект жёстко в epoll (`#error` если нет `HAVE_EPOLL`), `POLLING_KQUEUE/POLL`
        больше не определяются; `netpoll.cpp` 690→347 строк — kqueue/poll `PollTraits_t` и весь
        POLL-бэкенд физически удалены, остался только epoll (скобки/`#if`-баланс проверены)
- [x] 5.2 Вычистить `#if HAVE_KQUEUE` / `_WIN32` в `fileio.cpp`, `pollable_event.*` и связанном
      → удалены `_WIN32`-ветки `AutoFileOpen` и `sphPread` (`fileio.cpp`, остался POSIX/`pread`),
        `_WIN32`-ветка socketpair (`pollable_event.cpp`), kqueue-инклюд `<sys/event.h>` (`searchdha.cpp`).
        Широкий Windows-код в `searchdha.cpp` (≈18 `_WIN32`) — отдельная ось, не трогал (мёртв на Linux)
- [x] 5.3 Обновить CMake/детект платформы под Linux-only target
      → `CMakeLists.txt`: `FATAL_ERROR` если `CMAKE_SYSTEM_NAME != Linux`; epoll по-прежнему
        пробится (`check_function_exists(epoll_ctl HAVE_EPOLL)`) для compile-гварда в `netpoll.h`

## 6. Бенчи и валидация
- [ ] 6.1 Бенч-харность: cold/warm cache, конкуренция 1→N, индекс > RAM
- [ ] 6.2 Сравнение io_uring vs pread на одном бинаре (рантайм-флаг)
- [ ] 6.3 Снять QPS, p50/p95/p99, `aqu-sz` (iostat), CPU idle
- [ ] 6.4 Проверка фолбэка: старое ядро / seccomp-контейнер не падает, откат на pread
- [ ] 6.5 Корректность: существующие тесты поиска зелёные на обоих путях
