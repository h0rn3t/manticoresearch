# Async-чтение doclists/hitlists через io_uring (Linux-only)

## Why

Самые горячие при полнотекстовом матчинге структуры — **doclists** и **hitlists** — по
умолчанию читаются в режиме `FileAccess_e::FILE` через блокирующий `sphPread()`
([fileio.cpp:308](../../../src/fileio.cpp#L308), `FileReader_c::FillBuffer`). Чтение
происходит внутри корутины: пока `pread` крутится в ядре, worker-поток простаивает, а
другие запросы ждут свободного воркера.

На холодном page cache и датасетах больше RAM это ограничивает throughput: число
одновременных дисковых запросов упирается в число воркеров (низкая глубина очереди), и
NVMe не раскрывается — его сила в параллельных запросах (QD32+), а не в latency одного.

io_uring позволяет превратить блокирующий `pread` в `submit → yield → resume`: корутина
отправляет чтение в кольцо и засыпает, освобождая worker-поток под другие запросы;
завершение будит фибру. Глубина очереди диска растёт без роста числа потоков.

Цель деплоя — **только Linux**, поэтому переносимость на kqueue/poll/Windows для целевой
сборки не требуется и убирается.

## What Changes

- **Новый io_uring-backend для дискового чтения**: модуль управления кольцом (ring
  lifecycle), submit и выделенный io-поток-reaper (`io_uring_wait_cqe` → `Waker.Wake()`).
- **`FileReader_c::FillBuffer`** получает async-путь: под корутиной — submit+yield в
  io_uring; вне корутины или при отключённом io_uring — текущий `sphPread`.
- **Корутинный хелпер** `SubmitReadAndYield(fd, buf, len, off)` поверх существующего
  паттерна `Waker_c` / `YieldWith` (образец — `SuspendAndWaitUntil`,
  [coroutine.cpp:973](../../../src/coroutine.cpp#L973)).
- **Fallback за флагом**: `sphPread`-ветка сохраняется под compile/runtime-флагом
  (`searchd.io_uring = 1|0`, авто-детект `io_uring_setup`). Нужна как бэйзлайн для бенчей
  и как страховка в seccomp-ограниченных контейнерах, где `io_uring_setup` запрещён.
- **Дроп не-Linux бэкендов** (отдельная под-задача того же change): из `netpoll.cpp` и
  связанного кода убираются ветки `NETPOLL_KQUEUE` / `NETPOLL_POLL` / Windows; целевая
  платформа — Linux + epoll.
- **CMake**: поиск `liburing`, `HAVE_IO_URING`, опция сборки.
- **Бенч-харность**: сценарии cold/warm cache, рост конкуренции, индекс > RAM.

## Impact

- Affected specs: `file-io` (новая capability — режим дискового чтения и его контракт).
- Affected code: `src/fileio.cpp`, `src/fileio.h`, `src/datareader.cpp`, `src/coroutine.*`,
  новый модуль io_uring-backend, `src/netpoll.cpp` (дроп бэкендов), `CMakeLists.txt`,
  парсинг конфига `searchd`.
- **Платформа**: целевая сборка становится Linux-only. Внутри Linux сохраняется фолбэк на
  `pread` (старые ядра, seccomp-контейнеры).
- **Совместимость данных**: формат индексов не меняется — меняется только способ чтения.
- **Риск**: throttling в async-пути (`g_tThrottle`, тот самый `// FIXME!` на
  [fileio.cpp:308](../../../src/fileio.cpp#L308)) требует пересмотра.
