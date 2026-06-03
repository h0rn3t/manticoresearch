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
- [ ] 1.3 Опция конфига `searchd.io_uring = 1|0` (дефолт — авто/вкл)

## 2. io_uring backend (новый модуль) — `src/iouring.{h,cpp}`, верифицирован запуском
- [x] 2.1 Ring lifecycle: `StartIoUring`/`StopIoUring`, глубина очереди, NOP-sentinel teardown
- [x] 2.2 Submit-API: `SubmitRead` → `prep_read` + `user_data` = указатель на `Completion_t`
      (callback-based; корутинная склейка передаст callback с `Waker.Wake()`)
- [x] 2.3 Выделенный io-поток-reaper: `io_uring_wait_cqe` → разбор `user_data` → callback
- [x] 2.4 Thread-safe submit из воркеров (mutex вокруг SQ) — проверено 8 конкурентными чтениями
- [ ] 2.5 Ограничение inflight SQE (замена логики throttling) — частично: глубина кольца
      бьёт верхнюю границу, явный throttle-эквивалент ещё нужен

## 3. Корутинная интеграция
- [ ] 3.1 Хелпер `SubmitReadAndYield(fd, buf, len, off) -> bytes` поверх `Waker_c`/`YieldWith`
- [ ] 3.2 Корректная передача результата (число байт/ошибка) обратно в фибру
- [ ] 3.3 Обработка частичного чтения и ошибок (errno из CQE res)

## 4. FileReader интеграция
- [ ] 4.1 `FileReader_c::FillBuffer`: async-путь под корутиной, pread — иначе/при выкл io_uring
- [ ] 4.2 Проброс «async-capable» через `DirectFileReader_c`/`DirectFactory_c`
- [ ] 4.3 Учёт IO-статистики (`CSphIOStats`) в async-пути

## 5. Дроп не-Linux бэкендов (Linux-only)
- [ ] 5.1 `netpoll.cpp`: убрать `NETPOLL_KQUEUE`/`NETPOLL_POLL`/Windows, оставить epoll
- [ ] 5.2 Вычистить `#if HAVE_KQUEUE` / `_WIN32` в `fileio.cpp`, `pollable_event.*` и связанном
- [ ] 5.3 Обновить CMake/детект платформы под Linux-only target

## 6. Бенчи и валидация
- [ ] 6.1 Бенч-харность: cold/warm cache, конкуренция 1→N, индекс > RAM
- [ ] 6.2 Сравнение io_uring vs pread на одном бинаре (рантайм-флаг)
- [ ] 6.3 Снять QPS, p50/p95/p99, `aqu-sz` (iostat), CPU idle
- [ ] 6.4 Проверка фолбэка: старое ядро / seccomp-контейнер не падает, откат на pread
- [ ] 6.5 Корректность: существующие тесты поиска зелёные на обоих путях
