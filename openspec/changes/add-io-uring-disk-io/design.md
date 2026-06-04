# Design: io_uring async-чтение doclists/hitlists

## Контекст

Manticore — fiber/корутинный движок (`coroutine.cpp`) поверх `ThreadPool_c`
(`threadutils.cpp`). Дисковое чтение разделено на два семейства ридеров
(`datareader.cpp`):

- `DirectFileReader_c` → `FileReader_c` → буферизованный **`sphPread`** (режим `FILE`).
- `ThinMMapReader_c` → mmap + page fault (режимы `MMAP*`, `MLOCK`).

Дефолты доступа ([indexsettings.h:351-355](../../../src/indexsettings.h#L351-L355)):

| Данные   | Режим           | Механизм      |
|----------|-----------------|---------------|
| attrs    | `MMAP_PREREAD`  | mmap          |
| blob     | `MMAP_PREREAD`  | mmap          |
| doclists | `FILE`          | **pread** ← цель |
| hitlists | `FILE`          | **pread** ← цель |
| dict     | `MMAP_PREREAD`  | mmap          |

io_uring **не перехватывает page fault**, поэтому mmap-режимы (attrs/blob/dict) вне
скоупа: их async-чтение потребовало бы ухода с mmap на read(), что меняет модель
хранения. Скоуп — ровно `FILE`-путь (doclists/hitlists).

## Точка интеграции

Единственный блокирующий хот-спот — `FileReader_c::FillBuffer`,
[fileio.cpp:308](../../../src/fileio.cpp#L308):

```cpp
m_iBuffUsed = sphPread ( m_iFD, m_pBuff, iReadLen, iNewPos ); // FIXME! what about throttling?
```

Готовый образец async-suspend — `SuspendAndWaitUntil`,
[coroutine.cpp:973](../../../src/coroutine.cpp#L973): создаётся `Waker_c`, регистрируется
коллбэк-будилка, затем `YieldWith` усыпляет фибру; пробуждение приходит из другого
контекста через `Waker.Wake() → Reschedule`. io_uring повторяет этот паттерн один-в-один,
где роль «будилки» играет CQE-completion.

```
СЕЙЧАС:  worker: [submit pread]──BLOCK в ядре──[matching]
              N воркеров = max N inflight чтений

io_uring: worker: [submit SQE]→YieldWith→(берёт др. запрос)
              io-thread: wait_cqe → Waker.Wake() → фибра резюмится
              1 воркер обслуживает много inflight чтений → растёт QD
```

## Решения

### 1. Fallback — оставить за флагом (НЕ удалять сразу)

`sphPread`-ветка сохраняется под compile-флагом `HAVE_IO_URING` и runtime-опцией
`searchd.io_uring`. Авто-детект: пробный `io_uring_setup`/`io_uring_queue_init`; при
EPERM/ENOSYS — откат на pread с варнингом в лог.

Обоснование:
- **Бэйзлайн для бенчей** — нельзя доказать «+X% QPS», если удалить то, с чем сравниваем.
- **seccomp/контейнеры** — дефолтные docker/k8s-профили часто режут `io_uring_setup`;
  без фолбэка бинарь не стартует даже на новом ядре.
- Выпил pread-ветки — отдельный коммит на потом, когда бенчи подтвердят профит и деплой
  будет зафиксирован.

io_uring — **дефолт и основной путь**; pread — запасной.

### 2. Reaper — выделенный io-поток

Один (или небольшой пул) выделенный поток: `io_uring_wait_cqe` в цикле → по `user_data`
достаёт `Waker_c` и вызывает `Wake()`.

Обоснование: фибра может мигрировать между воркерами (`MoveTo`,
[coroutine.h:243](../../../src/coroutine.h#L243)), поэтому thread-local ring на воркер
(вариант C) рискует получить completion «не на том» кольце. Выделенный поток
изолирует submit/reaping от воркеров и проще для рассуждений. Submit из воркеров в общее
кольцо защищается локом вокруг SQ (или per-shard кольца). Оптимизация до thread-local
колец — потом, если профиль покажет contention на submit-локе.

Отвергнутые альтернативы:
- **netpoll-eventfd**: переиспользует epoll-цикл, но делает netpoll критичнее и тащит
  per-ring lock в сетевой путь. Отложено.
- **thread-local ring на воркер**: лучший scaling, но конфликтует с миграцией фибр.

### 3. Scope — широкий дроп (Linux-only)

Заодно убираются не-Linux бэкенды:
- `netpoll.cpp`: ветки `NETPOLL_KQUEUE`, `NETPOLL_POLL`, Windows → остаётся epoll.
- Соответствующие `#if HAVE_KQUEUE` / `_WIN32` в `fileio.cpp`, `pollable_event.*` и пр.

Это самостоятельная и самая объёмная часть change'а; на сам io_uring-путь она не влияет,
но фиксирует целевую платформу.

## Open Questions

- **Throttling в async-пути**: как ложится `g_tThrottle`/`sphReadThrottled` на submit в
  кольцо? Вариант — ограничивать число inflight SQE как аналог троттла.
- **Размер read-буфера vs SQE**: текущий `FillBuffer` читает `iReadLen` (read_buffer_docs/
  hits). Большие чтения, возможно, бить на несколько SQE или линковать (IOSQE_IO_LINK)?
- **Минимальная версия ядра**: какой floor фиксируем для опкодов чтения (5.10? 5.19? 6.x)?
- **Submit-батчинг**: копить ли SQE и делать один `io_uring_submit` на пачку, или submit
  на каждое чтение? Влияет на syscall-оверхед vs latency.
- **Регистрация fd/буферов** (`IOSQE_FIXED_FILE`, registered buffers): даёт прирост, но
  усложняет жизненный цикл — в первой версии или потом?

## Бенч-план

| Сценарий                     | Ожидание                        |
|------------------------------|---------------------------------|
| Cold cache (`drop_caches`)   | большой Δ throughput            |
| Warm cache                   | ~0 (sanity: не хуже pread)      |
| Конкуренция 1 → N запросов   | растёт с глубиной очереди       |
| Индекс >> RAM                | главный целевой сценарий        |
| p50 одиночного запроса       | может НЕ улучшиться (это норм)  |

Метрики: QPS, p50/p95/p99, `aqu-sz` из `iostat -x` (глубина очереди), CPU idle во время
поиска. Инструменты: `gbenches/` для микро + полноценная нагрузка (clickbench / реальный
датасет) с `drop_caches` между прогонами. Сравнение **io_uring vs pread на одном бинаре**
через рантайм-флаг.
